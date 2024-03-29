/*
 * bpf.c	BPF common code
 *
 *		This program is free software; you can distribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Daniel Borkmann <daniel@iogearbox.net>
 *		Jiri Pirko <jiri@resnulli.us>
 *		Alexei Starovoitov <ast@kernel.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <limits.h>
#include <assert.h>

#ifdef HAVE_ELF
#include <libelf.h>
#include <gelf.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/sendfile.h>
#include <sys/resource.h>

#include <arpa/inet.h>

#include "utils.h"
#include "json_print.h"

#include "bpf_util.h"
#include "bpf_elf.h"
#include "bpf_scm.h"

struct bpf_prog_meta {
	const char *type;
	const char *subdir;
	const char *section;
	bool may_uds_export;
};

static const enum bpf_prog_type __bpf_types[] = {
	BPF_PROG_TYPE_SCHED_CLS,
	BPF_PROG_TYPE_SCHED_ACT,
	BPF_PROG_TYPE_XDP,
	BPF_PROG_TYPE_LWT_IN,
	BPF_PROG_TYPE_LWT_OUT,
	BPF_PROG_TYPE_LWT_XMIT,
};

static const struct bpf_prog_meta __bpf_prog_meta[] = {
	[BPF_PROG_TYPE_SCHED_CLS] = {
		.type		= "cls",
		.subdir		= "tc",
		.section	= ELF_SECTION_CLASSIFIER,
		.may_uds_export	= true,
	},
	[BPF_PROG_TYPE_SCHED_ACT] = {
		.type		= "act",
		.subdir		= "tc",
		.section	= ELF_SECTION_ACTION,
		.may_uds_export	= true,
	},
	[BPF_PROG_TYPE_XDP] = {
		.type		= "xdp",
		.subdir		= "xdp",
		.section	= ELF_SECTION_PROG,
	},
	[BPF_PROG_TYPE_LWT_IN] = {
		.type		= "lwt_in",
		.subdir		= "ip",
		.section	= ELF_SECTION_PROG,
	},
	[BPF_PROG_TYPE_LWT_OUT] = {
		.type		= "lwt_out",
		.subdir		= "ip",
		.section	= ELF_SECTION_PROG,
	},
	[BPF_PROG_TYPE_LWT_XMIT] = {
		.type		= "lwt_xmit",
		.subdir		= "ip",
		.section	= ELF_SECTION_PROG,
	},
};

static const char *bpf_prog_to_subdir(enum bpf_prog_type type)
{
	assert(type < ARRAY_SIZE(__bpf_prog_meta) &&
	       __bpf_prog_meta[type].subdir);
	return __bpf_prog_meta[type].subdir;
}

const char *bpf_prog_to_default_section(enum bpf_prog_type type)
{
	assert(type < ARRAY_SIZE(__bpf_prog_meta) &&
	       __bpf_prog_meta[type].section);
	return __bpf_prog_meta[type].section;
}

#ifdef HAVE_ELF
static int bpf_obj_open(const char *path, enum bpf_prog_type type,
			const char *sec, bool verbose);
#else
static int bpf_obj_open(const char *path, enum bpf_prog_type type,
			const char *sec, bool verbose)
{
	fprintf(stderr, "No ELF library support compiled in.\n");
	errno = ENOSYS;
	return -1;
}
#endif

static inline __u64 bpf_ptr_to_u64(const void *ptr)
{
	return (__u64)(unsigned long)ptr;
}

static int bpf(int cmd, union bpf_attr *attr, unsigned int size)
{
#ifdef __NR_bpf
	return syscall(__NR_bpf, cmd, attr, size);
#else
	fprintf(stderr, "No bpf syscall, kernel headers too old?\n");
	errno = ENOSYS;
	return -1;
#endif
}

static int bpf_map_update(int fd, const void *key, const void *value,
			  uint64_t flags)
{
	union bpf_attr attr = {};

	attr.map_fd = fd;
	attr.key = bpf_ptr_to_u64(key);
	attr.value = bpf_ptr_to_u64(value);
	attr.flags = flags;

	return bpf(BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr));
}

static int bpf_prog_fd_by_id(uint32_t id)
{
	union bpf_attr attr = {};

	attr.prog_id = id;

	return bpf(BPF_PROG_GET_FD_BY_ID, &attr, sizeof(attr));
}

static int bpf_prog_info_by_fd(int fd, struct bpf_prog_info *info,
			       uint32_t *info_len)
{
	union bpf_attr attr = {};
	int ret;

	attr.info.bpf_fd = fd;
	attr.info.info = bpf_ptr_to_u64(info);
	attr.info.info_len = *info_len;

	*info_len = 0;
	ret = bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr));
	if (!ret)
		*info_len = attr.info.info_len;

	return ret;
}

int bpf_dump_prog_info(FILE *f, uint32_t id)
{
	struct bpf_prog_info info = {};
	uint32_t len = sizeof(info);
	int fd, ret, dump_ok = 0;
	SPRINT_BUF(tmp);

	open_json_object("prog");
	print_uint(PRINT_ANY, "id", "id %u ", id);

	fd = bpf_prog_fd_by_id(id);
	if (fd < 0)
		goto out;

	ret = bpf_prog_info_by_fd(fd, &info, &len);
	if (!ret && len) {
		int jited = !!info.jited_prog_len;

		print_string(PRINT_ANY, "tag", "tag %s ",
			     hexstring_n2a(info.tag, sizeof(info.tag),
					   tmp, sizeof(tmp)));
		print_uint(PRINT_JSON, "jited", NULL, jited);
		if (jited && !is_json_context())
			fprintf(f, "jited ");
		dump_ok = 1;
	}

	close(fd);
out:
	close_json_object();
	return dump_ok;
}

static int bpf_parse_string(char *arg, bool from_file, __u16 *bpf_len,
			    char **bpf_string, bool *need_release,
			    const char separator)
{
	char sp;

	if (from_file) {
		size_t tmp_len, op_len = sizeof("65535 255 255 4294967295,");
		char *tmp_string, *pos, c_prev = ' ';
		FILE *fp;
		int c;

		tmp_len = sizeof("4096,") + BPF_MAXINSNS * op_len;
		tmp_string = pos = calloc(1, tmp_len);
		if (tmp_string == NULL)
			return -ENOMEM;

		fp = fopen(arg, "r");
		if (fp == NULL) {
			perror("Cannot fopen");
			free(tmp_string);
			return -ENOENT;
		}

		while ((c = fgetc(fp)) != EOF) {
			switch (c) {
			case '\n':
				if (c_prev != ',')
					*(pos++) = ',';
				c_prev = ',';
				break;
			case ' ':
			case '\t':
				if (c_prev != ' ')
					*(pos++) = c;
				c_prev = ' ';
				break;
			default:
				*(pos++) = c;
				c_prev = c;
			}
			if (pos - tmp_string == tmp_len)
				break;
		}

		if (!feof(fp)) {
			free(tmp_string);
			fclose(fp);
			return -E2BIG;
		}

		fclose(fp);
		*pos = 0;

		*need_release = true;
		*bpf_string = tmp_string;
	} else {
		*need_release = false;
		*bpf_string = arg;
	}

	if (sscanf(*bpf_string, "%hu%c", bpf_len, &sp) != 2 ||
	    sp != separator) {
		if (*need_release)
			free(*bpf_string);
		return -EINVAL;
	}

	return 0;
}

static int bpf_ops_parse(int argc, char **argv, struct sock_filter *bpf_ops,
			 bool from_file)
{
	char *bpf_string, *token, separator = ',';
	int ret = 0, i = 0;
	bool need_release;
	__u16 bpf_len = 0;

	if (argc < 1)
		return -EINVAL;
	if (bpf_parse_string(argv[0], from_file, &bpf_len, &bpf_string,
			     &need_release, separator))
		return -EINVAL;
	if (bpf_len == 0 || bpf_len > BPF_MAXINSNS) {
		ret = -EINVAL;
		goto out;
	}

	token = bpf_string;
	while ((token = strchr(token, separator)) && (++token)[0]) {
		if (i >= bpf_len) {
			fprintf(stderr, "Real program length exceeds encoded length parameter!\n");
			ret = -EINVAL;
			goto out;
		}

		if (sscanf(token, "%hu %hhu %hhu %u,",
			   &bpf_ops[i].code, &bpf_ops[i].jt,
			   &bpf_ops[i].jf, &bpf_ops[i].k) != 4) {
			fprintf(stderr, "Error at instruction %d!\n", i);
			ret = -EINVAL;
			goto out;
		}

		i++;
	}

	if (i != bpf_len) {
		fprintf(stderr, "Parsed program length is less than encoded length parameter!\n");
		ret = -EINVAL;
		goto out;
	}
	ret = bpf_len;
out:
	if (need_release)
		free(bpf_string);

	return ret;
}

void bpf_print_ops(FILE *f, struct rtattr *bpf_ops, __u16 len)
{
	struct sock_filter *ops = RTA_DATA(bpf_ops);
	int i;

	if (len == 0)
		return;

	fprintf(f, "bytecode \'%u,", len);

	for (i = 0; i < len - 1; i++)
		fprintf(f, "%hu %hhu %hhu %u,", ops[i].code, ops[i].jt,
			ops[i].jf, ops[i].k);

	fprintf(f, "%hu %hhu %hhu %u\'", ops[i].code, ops[i].jt,
		ops[i].jf, ops[i].k);
}

static void bpf_map_pin_report(const struct bpf_elf_map *pin,
			       const struct bpf_elf_map *obj)
{
	fprintf(stderr, "Map specification differs from pinned file!\n");

	if (obj->type != pin->type)
		fprintf(stderr, " - Type:         %u (obj) != %u (pin)\n",
			obj->type, pin->type);
	if (obj->size_key != pin->size_key)
		fprintf(stderr, " - Size key:     %u (obj) != %u (pin)\n",
			obj->size_key, pin->size_key);
	if (obj->size_value != pin->size_value)
		fprintf(stderr, " - Size value:   %u (obj) != %u (pin)\n",
			obj->size_value, pin->size_value);
	if (obj->max_elem != pin->max_elem)
		fprintf(stderr, " - Max elems:    %u (obj) != %u (pin)\n",
			obj->max_elem, pin->max_elem);
	if (obj->flags != pin->flags)
		fprintf(stderr, " - Flags:        %#x (obj) != %#x (pin)\n",
			obj->flags, pin->flags);

	fprintf(stderr, "\n");
}

struct bpf_prog_data {
	unsigned int type;
	unsigned int jited;
};

struct bpf_map_ext {
	struct bpf_prog_data owner;
};

static int bpf_derive_elf_map_from_fdinfo(int fd, struct bpf_elf_map *map,
					  struct bpf_map_ext *ext)
{
	unsigned int val, owner_type = 0, owner_jited = 0;
	char file[PATH_MAX], buff[4096];
	FILE *fp;

	snprintf(file, sizeof(file), "/proc/%d/fdinfo/%d", getpid(), fd);
	memset(map, 0, sizeof(*map));

	fp = fopen(file, "r");
	if (!fp) {
		fprintf(stderr, "No procfs support?!\n");
		return -EIO;
	}

	while (fgets(buff, sizeof(buff), fp)) {
		if (sscanf(buff, "map_type:\t%u", &val) == 1)
			map->type = val;
		else if (sscanf(buff, "key_size:\t%u", &val) == 1)
			map->size_key = val;
		else if (sscanf(buff, "value_size:\t%u", &val) == 1)
			map->size_value = val;
		else if (sscanf(buff, "max_entries:\t%u", &val) == 1)
			map->max_elem = val;
		else if (sscanf(buff, "map_flags:\t%i", &val) == 1)
			map->flags = val;
		else if (sscanf(buff, "owner_prog_type:\t%i", &val) == 1)
			owner_type = val;
		else if (sscanf(buff, "owner_jited:\t%i", &val) == 1)
			owner_jited = val;
	}

	fclose(fp);
	if (ext) {
		memset(ext, 0, sizeof(*ext));
		ext->owner.type  = owner_type;
		ext->owner.jited = owner_jited;
	}

	return 0;
}

static int bpf_map_selfcheck_pinned(int fd, const struct bpf_elf_map *map,
				    struct bpf_map_ext *ext, int length,
				    enum bpf_prog_type type)
{
	struct bpf_elf_map tmp, zero = {};
	int ret;

	ret = bpf_derive_elf_map_from_fdinfo(fd, &tmp, ext);
	if (ret < 0)
		return ret;

	/* The decision to reject this is on kernel side eventually, but
	 * at least give the user a chance to know what's wrong.
	 */
	if (ext->owner.type && ext->owner.type != type)
		fprintf(stderr, "Program array map owner types differ: %u (obj) != %u (pin)\n",
			type, ext->owner.type);

	if (!memcmp(&tmp, map, length)) {
		return 0;
	} else {
		/* If kernel doesn't have eBPF-related fdinfo, we cannot do much,
		 * so just accept it. We know we do have an eBPF fd and in this
		 * case, everything is 0. It is guaranteed that no such map exists
		 * since map type of 0 is unloadable BPF_MAP_TYPE_UNSPEC.
		 */
		if (!memcmp(&tmp, &zero, length))
			return 0;

		bpf_map_pin_report(&tmp, map);
		return -EINVAL;
	}
}

static int bpf_mnt_fs(const char *target)
{
	bool bind_done = false;

	while (mount("", target, "none", MS_PRIVATE | MS_REC, NULL)) {
		if (errno != EINVAL || bind_done) {
			fprintf(stderr, "mount --make-private %s failed: %s\n",
				target,	strerror(errno));
			return -1;
		}

		if (mount(target, target, "none", MS_BIND, NULL)) {
			fprintf(stderr, "mount --bind %s %s failed: %s\n",
				target,	target, strerror(errno));
			return -1;
		}

		bind_done = true;
	}

	if (mount("bpf", target, "bpf", 0, "mode=0700")) {
		fprintf(stderr, "mount -t bpf bpf %s failed: %s\n",
			target,	strerror(errno));
		return -1;
	}

	return 0;
}

static int bpf_mnt_check_target(const char *target)
{
	struct stat sb = {};
	int ret;

	ret = stat(target, &sb);
	if (ret) {
		ret = mkdir(target, S_IRWXU);
		if (ret) {
			fprintf(stderr, "mkdir %s failed: %s\n", target,
				strerror(errno));
			return ret;
		}
	}

	return 0;
}

static int bpf_valid_mntpt(const char *mnt, unsigned long magic)
{
	struct statfs st_fs;

	if (statfs(mnt, &st_fs) < 0)
		return -ENOENT;
	if ((unsigned long)st_fs.f_type != magic)
		return -ENOENT;

	return 0;
}

static const char *bpf_find_mntpt_single(unsigned long magic, char *mnt,
					 int len, const char *mntpt)
{
	int ret;

	ret = bpf_valid_mntpt(mntpt, magic);
	if (!ret) {
		strlcpy(mnt, mntpt, len);
		return mnt;
	}

	return NULL;
}

static const char *bpf_find_mntpt(const char *fstype, unsigned long magic,
				  char *mnt, int len,
				  const char * const *known_mnts)
{
	const char * const *ptr;
	char type[100];
	FILE *fp;

	if (known_mnts) {
		ptr = known_mnts;
		while (*ptr) {
			if (bpf_find_mntpt_single(magic, mnt, len, *ptr))
				return mnt;
			ptr++;
		}
	}

	if (len != PATH_MAX)
		return NULL;

	fp = fopen("/proc/mounts", "r");
	if (fp == NULL)
		return NULL;

	while (fscanf(fp, "%*s %" textify(PATH_MAX) "s %99s %*s %*d %*d\n",
		      mnt, type) == 2) {
		if (strcmp(type, fstype) == 0)
			break;
	}

	fclose(fp);
	if (strcmp(type, fstype) != 0)
		return NULL;

	return mnt;
}

int bpf_trace_pipe(void)
{
	char tracefs_mnt[PATH_MAX] = TRACE_DIR_MNT;
	static const char * const tracefs_known_mnts[] = {
		TRACE_DIR_MNT,
		"/sys/kernel/debug/tracing",
		"/tracing",
		"/trace",
		0,
	};
	int fd_in, fd_out = STDERR_FILENO;
	char tpipe[PATH_MAX];
	const char *mnt;

	mnt = bpf_find_mntpt("tracefs", TRACEFS_MAGIC, tracefs_mnt,
			     sizeof(tracefs_mnt), tracefs_known_mnts);
	if (!mnt) {
		fprintf(stderr, "tracefs not mounted?\n");
		return -1;
	}

	snprintf(tpipe, sizeof(tpipe), "%s/trace_pipe", mnt);

	fd_in = open(tpipe, O_RDONLY);
	if (fd_in < 0)
		return -1;

	fprintf(stderr, "Running! Hang up with ^C!\n\n");
	while (1) {
		static char buff[4096];
		ssize_t ret;

		ret = read(fd_in, buff, sizeof(buff));
		if (ret > 0 && write(fd_out, buff, ret) == ret)
			continue;
		break;
	}

	close(fd_in);
	return -1;
}

static int bpf_gen_global(const char *bpf_sub_dir)
{
	char bpf_glo_dir[PATH_MAX];
	int ret;

	snprintf(bpf_glo_dir, sizeof(bpf_glo_dir), "%s/%s/",
		 bpf_sub_dir, BPF_DIR_GLOBALS);

	ret = mkdir(bpf_glo_dir, S_IRWXU);
	if (ret && errno != EEXIST) {
		fprintf(stderr, "mkdir %s failed: %s\n", bpf_glo_dir,
			strerror(errno));
		return ret;
	}

	return 0;
}

static int bpf_gen_master(const char *base, const char *name)
{
	char bpf_sub_dir[PATH_MAX];
	int ret;

	snprintf(bpf_sub_dir, sizeof(bpf_sub_dir), "%s%s/", base, name);

	ret = mkdir(bpf_sub_dir, S_IRWXU);
	if (ret && errno != EEXIST) {
		fprintf(stderr, "mkdir %s failed: %s\n", bpf_sub_dir,
			strerror(errno));
		return ret;
	}

	return bpf_gen_global(bpf_sub_dir);
}

static int bpf_slave_via_bind_mnt(const char *full_name,
				  const char *full_link)
{
	int ret;

	ret = mkdir(full_name, S_IRWXU);
	if (ret) {
		assert(errno != EEXIST);
		fprintf(stderr, "mkdir %s failed: %s\n", full_name,
			strerror(errno));
		return ret;
	}

	ret = mount(full_link, full_name, "none", MS_BIND, NULL);
	if (ret) {
		rmdir(full_name);
		fprintf(stderr, "mount --bind %s %s failed: %s\n",
			full_link, full_name, strerror(errno));
	}

	return ret;
}

static int bpf_gen_slave(const char *base, const char *name,
			 const char *link)
{
	char bpf_lnk_dir[PATH_MAX];
	char bpf_sub_dir[PATH_MAX];
	struct stat sb = {};
	int ret;

	snprintf(bpf_lnk_dir, sizeof(bpf_lnk_dir), "%s%s/", base, link);
	snprintf(bpf_sub_dir, sizeof(bpf_sub_dir), "%s%s",  base, name);

	ret = symlink(bpf_lnk_dir, bpf_sub_dir);
	if (ret) {
		if (errno != EEXIST) {
			if (errno != EPERM) {
				fprintf(stderr, "symlink %s failed: %s\n",
					bpf_sub_dir, strerror(errno));
				return ret;
			}

			return bpf_slave_via_bind_mnt(bpf_sub_dir,
						      bpf_lnk_dir);
		}

		ret = lstat(bpf_sub_dir, &sb);
		if (ret) {
			fprintf(stderr, "lstat %s failed: %s\n",
				bpf_sub_dir, strerror(errno));
			return ret;
		}

		if ((sb.st_mode & S_IFMT) != S_IFLNK)
			return bpf_gen_global(bpf_sub_dir);
	}

	return 0;
}

static int bpf_gen_hierarchy(const char *base)
{
	int ret, i;

	ret = bpf_gen_master(base, bpf_prog_to_subdir(__bpf_types[0]));
	for (i = 1; i < ARRAY_SIZE(__bpf_types) && !ret; i++)
		ret = bpf_gen_slave(base,
				    bpf_prog_to_subdir(__bpf_types[i]),
				    bpf_prog_to_subdir(__bpf_types[0]));
	return ret;
}

static const char *bpf_get_work_dir(enum bpf_prog_type type)
{
	static char bpf_tmp[PATH_MAX] = BPF_DIR_MNT;
	static char bpf_wrk_dir[PATH_MAX];
	static const char *mnt;
	static bool bpf_mnt_cached;
	const char *mnt_env = getenv(BPF_ENV_MNT);
	static const char * const bpf_known_mnts[] = {
		BPF_DIR_MNT,
		"/bpf",
		0,
	};
	int ret;

	if (bpf_mnt_cached) {
		const char *out = mnt;

		if (out && type) {
			snprintf(bpf_tmp, sizeof(bpf_tmp), "%s%s/",
				 out, bpf_prog_to_subdir(type));
			out = bpf_tmp;
		}
		return out;
	}

	if (mnt_env)
		mnt = bpf_find_mntpt_single(BPF_FS_MAGIC, bpf_tmp,
					    sizeof(bpf_tmp), mnt_env);
	else
		mnt = bpf_find_mntpt("bpf", BPF_FS_MAGIC, bpf_tmp,
				     sizeof(bpf_tmp), bpf_known_mnts);
	if (!mnt) {
		mnt = mnt_env ? : BPF_DIR_MNT;
		ret = bpf_mnt_check_target(mnt);
		if (!ret)
			ret = bpf_mnt_fs(mnt);
		if (ret) {
			mnt = NULL;
			goto out;
		}
	}

	snprintf(bpf_wrk_dir, sizeof(bpf_wrk_dir), "%s/", mnt);

	ret = bpf_gen_hierarchy(bpf_wrk_dir);
	if (ret) {
		mnt = NULL;
		goto out;
	}

	mnt = bpf_wrk_dir;
out:
	bpf_mnt_cached = true;
	return mnt;
}

static int bpf_obj_get(const char *pathname, enum bpf_prog_type type)
{
	union bpf_attr attr = {};
	char tmp[PATH_MAX];

	if (strlen(pathname) > 2 && pathname[0] == 'm' &&
	    pathname[1] == ':' && bpf_get_work_dir(type)) {
		snprintf(tmp, sizeof(tmp), "%s/%s",
			 bpf_get_work_dir(type), pathname + 2);
		pathname = tmp;
	}

	attr.pathname = bpf_ptr_to_u64(pathname);

	return bpf(BPF_OBJ_GET, &attr, sizeof(attr));
}

static int bpf_obj_pinned(const char *pathname, enum bpf_prog_type type)
{
	int prog_fd = bpf_obj_get(pathname, type);

	if (prog_fd < 0)
		fprintf(stderr, "Couldn\'t retrieve pinned program \'%s\': %s\n",
			pathname, strerror(errno));
	return prog_fd;
}

enum bpf_mode {
	CBPF_BYTECODE,
	CBPF_FILE,
	EBPF_OBJECT,
	EBPF_PINNED,
	BPF_MODE_MAX,
};

static int bpf_parse(enum bpf_prog_type *type, enum bpf_mode *mode,
		     struct bpf_cfg_in *cfg, const bool *opt_tbl)
{
	const char *file, *section, *uds_name;
	bool verbose = false;
	int i, ret, argc;
	char **argv;

	argv = cfg->argv;
	argc = cfg->argc;

	if (opt_tbl[CBPF_BYTECODE] &&
	    (matches(*argv, "bytecode") == 0 ||
	     strcmp(*argv, "bc") == 0)) {
		*mode = CBPF_BYTECODE;
	} else if (opt_tbl[CBPF_FILE] &&
		   (matches(*argv, "bytecode-file") == 0 ||
		    strcmp(*argv, "bcf") == 0)) {
		*mode = CBPF_FILE;
	} else if (opt_tbl[EBPF_OBJECT] &&
		   (matches(*argv, "object-file") == 0 ||
		    strcmp(*argv, "obj") == 0)) {
		*mode = EBPF_OBJECT;
	} else if (opt_tbl[EBPF_PINNED] &&
		   (matches(*argv, "object-pinned") == 0 ||
		    matches(*argv, "pinned") == 0 ||
		    matches(*argv, "fd") == 0)) {
		*mode = EBPF_PINNED;
	} else {
		fprintf(stderr, "What mode is \"%s\"?\n", *argv);
		return -1;
	}

	NEXT_ARG();
	file = section = uds_name = NULL;
	if (*mode == EBPF_OBJECT || *mode == EBPF_PINNED) {
		file = *argv;
		NEXT_ARG_FWD();

		if (*type == BPF_PROG_TYPE_UNSPEC) {
			if (argc > 0 && matches(*argv, "type") == 0) {
				NEXT_ARG();
				for (i = 0; i < ARRAY_SIZE(__bpf_prog_meta);
				     i++) {
					if (!__bpf_prog_meta[i].type)
						continue;
					if (!matches(*argv,
						     __bpf_prog_meta[i].type)) {
						*type = i;
						break;
					}
				}

				if (*type == BPF_PROG_TYPE_UNSPEC) {
					fprintf(stderr, "What type is \"%s\"?\n",
						*argv);
					return -1;
				}
				NEXT_ARG_FWD();
			} else {
				*type = BPF_PROG_TYPE_SCHED_CLS;
			}
		}

		section = bpf_prog_to_default_section(*type);
		if (argc > 0 && matches(*argv, "section") == 0) {
			NEXT_ARG();
			section = *argv;
			NEXT_ARG_FWD();
		}

		if (__bpf_prog_meta[*type].may_uds_export) {
			uds_name = getenv(BPF_ENV_UDS);
			if (argc > 0 && !uds_name &&
			    matches(*argv, "export") == 0) {
				NEXT_ARG();
				uds_name = *argv;
				NEXT_ARG_FWD();
			}
		}

		if (argc > 0 && matches(*argv, "verbose") == 0) {
			verbose = true;
			NEXT_ARG_FWD();
		}

		PREV_ARG();
	}

	if (*mode == CBPF_BYTECODE || *mode == CBPF_FILE)
		ret = bpf_ops_parse(argc, argv, cfg->ops, *mode == CBPF_FILE);
	else if (*mode == EBPF_OBJECT)
		ret = bpf_obj_open(file, *type, section, verbose);
	else if (*mode == EBPF_PINNED)
		ret = bpf_obj_pinned(file, *type);
	else
		return -1;

	cfg->object  = file;
	cfg->section = section;
	cfg->uds     = uds_name;
	cfg->argc    = argc;
	cfg->argv    = argv;

	return ret;
}

static int bpf_parse_opt_tbl(enum bpf_prog_type type, struct bpf_cfg_in *cfg,
			     const struct bpf_cfg_ops *ops, void *nl,
			     const bool *opt_tbl)
{
	struct sock_filter opcodes[BPF_MAXINSNS];
	char annotation[256];
	enum bpf_mode mode;
	int ret;

	cfg->ops = opcodes;
	ret = bpf_parse(&type, &mode, cfg, opt_tbl);
	cfg->ops = NULL;
	if (ret < 0)
		return ret;

	if (mode == CBPF_BYTECODE || mode == CBPF_FILE)
		ops->cbpf_cb(nl, opcodes, ret);
	if (mode == EBPF_OBJECT || mode == EBPF_PINNED) {
		snprintf(annotation, sizeof(annotation), "%s:[%s]",
			 basename(cfg->object), mode == EBPF_PINNED ?
			 "*fsobj" : cfg->section);
		ops->ebpf_cb(nl, ret, annotation);
	}

	return 0;
}

int bpf_parse_common(enum bpf_prog_type type, struct bpf_cfg_in *cfg,
		     const struct bpf_cfg_ops *ops, void *nl)
{
	bool opt_tbl[BPF_MODE_MAX] = {};

	if (ops->cbpf_cb) {
		opt_tbl[CBPF_BYTECODE] = true;
		opt_tbl[CBPF_FILE]     = true;
	}

	if (ops->ebpf_cb) {
		opt_tbl[EBPF_OBJECT]   = true;
		opt_tbl[EBPF_PINNED]   = true;
	}

	return bpf_parse_opt_tbl(type, cfg, ops, nl, opt_tbl);
}

int bpf_graft_map(const char *map_path, uint32_t *key, int argc, char **argv)
{
	enum bpf_prog_type type = BPF_PROG_TYPE_UNSPEC;
	const bool opt_tbl[BPF_MODE_MAX] = {
		[EBPF_OBJECT]	= true,
		[EBPF_PINNED]	= true,
	};
	const struct bpf_elf_map test = {
		.type		= BPF_MAP_TYPE_PROG_ARRAY,
		.size_key	= sizeof(int),
		.size_value	= sizeof(int),
	};
	struct bpf_cfg_in cfg = {
		.argc		= argc,
		.argv		= argv,
	};
	struct bpf_map_ext ext = {};
	int ret, prog_fd, map_fd;
	enum bpf_mode mode;
	uint32_t map_key;

	prog_fd = bpf_parse(&type, &mode, &cfg, opt_tbl);
	if (prog_fd < 0)
		return prog_fd;
	if (key) {
		map_key = *key;
	} else {
		ret = sscanf(cfg.section, "%*i/%i", &map_key);
		if (ret != 1) {
			fprintf(stderr, "Couldn\'t infer map key from section name! Please provide \'key\' argument!\n");
			ret = -EINVAL;
			goto out_prog;
		}
	}

	map_fd = bpf_obj_get(map_path, type);
	if (map_fd < 0) {
		fprintf(stderr, "Couldn\'t retrieve pinned map \'%s\': %s\n",
			map_path, strerror(errno));
		ret = map_fd;
		goto out_prog;
	}

	ret = bpf_map_selfcheck_pinned(map_fd, &test, &ext,
				       offsetof(struct bpf_elf_map, max_elem),
				       type);
	if (ret < 0) {
		fprintf(stderr, "Map \'%s\' self-check failed!\n", map_path);
		goto out_map;
	}

	ret = bpf_map_update(map_fd, &map_key, &prog_fd, BPF_ANY);
	if (ret < 0)
		fprintf(stderr, "Map update failed: %s\n", strerror(errno));
out_map:
	close(map_fd);
out_prog:
	close(prog_fd);
	return ret;
}

int bpf_prog_attach_fd(int prog_fd, int target_fd, enum bpf_attach_type type)
{
	union bpf_attr attr = {};

	attr.target_fd = target_fd;
	attr.attach_bpf_fd = prog_fd;
	attr.attach_type = type;

	return bpf(BPF_PROG_ATTACH, &attr, sizeof(attr));
}

int bpf_prog_detach_fd(int target_fd, enum bpf_attach_type type)
{
	union bpf_attr attr = {};

	attr.target_fd = target_fd;
	attr.attach_type = type;

	return bpf(BPF_PROG_DETACH, &attr, sizeof(attr));
}

int bpf_prog_load(enum bpf_prog_type type, const struct bpf_insn *insns,
		  size_t size_insns, const char *license, char *log,
		  size_t size_log)
{
	union bpf_attr attr = {};

	attr.prog_type = type;
	attr.insns = bpf_ptr_to_u64(insns);
	attr.insn_cnt = size_insns / sizeof(struct bpf_insn);
	attr.license = bpf_ptr_to_u64(license);

	if (size_log > 0) {
		attr.log_buf = bpf_ptr_to_u64(log);
		attr.log_size = size_log;
		attr.log_level = 1;
	}

	return bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
}

#ifdef HAVE_ELF
struct bpf_elf_prog {
	enum bpf_prog_type	type;
	const struct bpf_insn	*insns;
	size_t			size;
	const char		*license;
};

struct bpf_hash_entry {
	unsigned int		pinning;
	const char		*subpath;
	struct bpf_hash_entry	*next;
};

struct bpf_config {
	unsigned int		jit_enabled;
};

struct bpf_elf_ctx {
	struct bpf_config	cfg;
	Elf			*elf_fd;
	GElf_Ehdr		elf_hdr;
	Elf_Data		*sym_tab;
	Elf_Data		*str_tab;
	int			obj_fd;
	int			map_fds[ELF_MAX_MAPS];
	struct bpf_elf_map	maps[ELF_MAX_MAPS];
	struct bpf_map_ext	maps_ext[ELF_MAX_MAPS];
	int			sym_num;
	int			map_num;
	int			map_len;
	bool			*sec_done;
	int			sec_maps;
	char			license[ELF_MAX_LICENSE_LEN];
	enum bpf_prog_type	type;
	bool			verbose;
	struct bpf_elf_st	stat;
	struct bpf_hash_entry	*ht[256];
	char			*log;
	size_t			log_size;
};

struct bpf_elf_sec_data {
	GElf_Shdr		sec_hdr;
	Elf_Data		*sec_data;
	const char		*sec_name;
};

struct bpf_map_data {
	int			*fds;
	const char		*obj;
	struct bpf_elf_st	*st;
	struct bpf_elf_map	*ent;
};

static __check_format_string(2, 3) void
bpf_dump_error(struct bpf_elf_ctx *ctx, const char *format, ...)
{
	va_list vl;

	va_start(vl, format);
	vfprintf(stderr, format, vl);
	va_end(vl);

	if (ctx->log && ctx->log[0]) {
		if (ctx->verbose) {
			fprintf(stderr, "%s\n", ctx->log);
		} else {
			unsigned int off = 0, len = strlen(ctx->log);

			if (len > BPF_MAX_LOG) {
				off = len - BPF_MAX_LOG;
				fprintf(stderr, "Skipped %u bytes, use \'verb\' option for the full verbose log.\n[...]\n",
					off);
			}
			fprintf(stderr, "%s\n", ctx->log + off);
		}

		memset(ctx->log, 0, ctx->log_size);
	}
}

static int bpf_log_realloc(struct bpf_elf_ctx *ctx)
{
	const size_t log_max = UINT_MAX >> 8;
	size_t log_size = ctx->log_size;
	void *ptr;

	if (!ctx->log) {
		log_size = 65536;
	} else if (log_size < log_max) {
		log_size <<= 1;
		if (log_size > log_max)
			log_size = log_max;
	} else {
		return -EINVAL;
	}

	ptr = realloc(ctx->log, log_size);
	if (!ptr)
		return -ENOMEM;

	ctx->log = ptr;
	ctx->log_size = log_size;

	return 0;
}

static int bpf_map_create(enum bpf_map_type type, uint32_t size_key,
			  uint32_t size_value, uint32_t max_elem,
			  uint32_t flags, int inner_fd)
{
	union bpf_attr attr = {};

	attr.map_type = type;
	attr.key_size = size_key;
	attr.value_size = inner_fd ? sizeof(int) : size_value;
	attr.max_entries = max_elem;
	attr.map_flags = flags;
	attr.inner_map_fd = inner_fd;

	return bpf(BPF_MAP_CREATE, &attr, sizeof(attr));
}

static int bpf_obj_pin(int fd, const char *pathname)
{
	union bpf_attr attr = {};

	attr.pathname = bpf_ptr_to_u64(pathname);
	attr.bpf_fd = fd;

	return bpf(BPF_OBJ_PIN, &attr, sizeof(attr));
}

static int bpf_obj_hash(const char *object, uint8_t *out, size_t len)
{
	struct sockaddr_alg alg = {
		.salg_family	= AF_ALG,
		.salg_type	= "hash",
		.salg_name	= "sha1",
	};
	int ret, cfd, ofd, ffd;
	struct stat stbuff;
	ssize_t size;

	if (!object || len != 20)
		return -EINVAL;

	cfd = socket(AF_ALG, SOCK_SEQPACKET, 0);

        if(cfd == -1){
            if(errno == EAFNOSUPPORT){
                //Unavailable, put whatever you want here
                fprintf(stderr, "#define AF_ALG_UNAVAILABLE\n");
            } else {
            //Unable to detect for some other error
            }
            } else { //AF_ALG is available
                fprintf(stderr, "#define AF_ALG_AVAILABLE\n");
        }

	if (cfd < 0) {
		fprintf(stderr, "Cannot get AF_ALG socket: %s\n",
			strerror(errno));
		return cfd;
	}

	ret = bind(cfd, (struct sockaddr *)&alg, sizeof(alg));
	if (ret < 0) {
		fprintf(stderr, "Error binding socket: %s\n", strerror(errno));
		goto out_cfd;
	}

	ofd = accept(cfd, NULL, 0);
	if (ofd < 0) {
		fprintf(stderr, "Error accepting socket: %s\n",
			strerror(errno));
		ret = ofd;
		goto out_cfd;
	}

	ffd = open(object, O_RDONLY);
	if (ffd < 0) {
		fprintf(stderr, "Error opening object %s: %s\n",
			object, strerror(errno));
		ret = ffd;
		goto out_ofd;
	}

	ret = fstat(ffd, &stbuff);
	if (ret < 0) {
		fprintf(stderr, "Error doing fstat: %s\n",
			strerror(errno));
		goto out_ffd;
	}

	size = sendfile(ofd, ffd, NULL, stbuff.st_size);
	if (size != stbuff.st_size) {
		/*fprintf(stderr, "Error from sendfile (%zd vs %zu bytes): %s\n",
			size, stbuff.st_size, strerror(errno));*/
		fprintf(stderr, "xdp-on-android bug\n");
		ret = -1;
		goto out_ffd;
	}

	size = read(ofd, out, len);
	if (size != len) {
		fprintf(stderr, "Error from read (%zd vs %zu bytes): %s\n",
			size, len, strerror(errno));
		ret = -1;
	} else {
		ret = 0;
	}
out_ffd:
	close(ffd);
out_ofd:
	close(ofd);
out_cfd:
	close(cfd);
	return ret;
}

static const char *bpf_get_obj_uid(const char *pathname)
{
	static bool bpf_uid_cached;
	static char bpf_uid[64];
	uint8_t tmp[20];
	int ret;

	if (bpf_uid_cached)
		goto done;

	ret = bpf_obj_hash(pathname, tmp, sizeof(tmp));
	if (ret) {
		fprintf(stderr, "Object hashing failed!\n");
		return NULL;
	}

	hexstring_n2a(tmp, sizeof(tmp), bpf_uid, sizeof(bpf_uid));
	bpf_uid_cached = true;
done:
	return bpf_uid;
}

static int bpf_init_env(const char *pathname)
{
	struct rlimit limit = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};

	/* Don't bother in case we fail! */
	setrlimit(RLIMIT_MEMLOCK, &limit);

	if (!bpf_get_work_dir(BPF_PROG_TYPE_UNSPEC)) {
		fprintf(stderr, "Continuing without mounted eBPF fs. Too old kernel?\n");
		return 0;
	}

	if (!bpf_get_obj_uid(pathname))
		return -1;

	return 0;
}

static const char *bpf_custom_pinning(const struct bpf_elf_ctx *ctx,
				      uint32_t pinning)
{
	struct bpf_hash_entry *entry;

	entry = ctx->ht[pinning & (ARRAY_SIZE(ctx->ht) - 1)];
	while (entry && entry->pinning != pinning)
		entry = entry->next;

	return entry ? entry->subpath : NULL;
}

static bool bpf_no_pinning(const struct bpf_elf_ctx *ctx,
			   uint32_t pinning)
{
	switch (pinning) {
	case PIN_OBJECT_NS:
	case PIN_GLOBAL_NS:
		return false;
	case PIN_NONE:
		return true;
	default:
		return !bpf_custom_pinning(ctx, pinning);
	}
}

static void bpf_make_pathname(char *pathname, size_t len, const char *name,
			      const struct bpf_elf_ctx *ctx, uint32_t pinning)
{
	switch (pinning) {
	case PIN_OBJECT_NS:
		snprintf(pathname, len, "%s/%s/%s",
			 bpf_get_work_dir(ctx->type),
			 bpf_get_obj_uid(NULL), name);
		break;
	case PIN_GLOBAL_NS:
		snprintf(pathname, len, "%s/%s/%s",
			 bpf_get_work_dir(ctx->type),
			 BPF_DIR_GLOBALS, name);
		break;
	default:
		snprintf(pathname, len, "%s/../%s/%s",
			 bpf_get_work_dir(ctx->type),
			 bpf_custom_pinning(ctx, pinning), name);
		break;
	}
}

static int bpf_probe_pinned(const char *name, const struct bpf_elf_ctx *ctx,
			    uint32_t pinning)
{
	char pathname[PATH_MAX];

	if (bpf_no_pinning(ctx, pinning) || !bpf_get_work_dir(ctx->type))
		return 0;

	bpf_make_pathname(pathname, sizeof(pathname), name, ctx, pinning);
	return bpf_obj_get(pathname, ctx->type);
}

static int bpf_make_obj_path(const struct bpf_elf_ctx *ctx)
{
	char tmp[PATH_MAX];
	int ret;

	snprintf(tmp, sizeof(tmp), "%s/%s", bpf_get_work_dir(ctx->type),
		 bpf_get_obj_uid(NULL));

	ret = mkdir(tmp, S_IRWXU);
	if (ret && errno != EEXIST) {
		fprintf(stderr, "mkdir %s failed: %s\n", tmp, strerror(errno));
		return ret;
	}

	return 0;
}

static int bpf_make_custom_path(const struct bpf_elf_ctx *ctx,
				const char *todo)
{
	char tmp[PATH_MAX], rem[PATH_MAX], *sub;
	int ret;

	snprintf(tmp, sizeof(tmp), "%s/../", bpf_get_work_dir(ctx->type));
	snprintf(rem, sizeof(rem), "%s/", todo);
	sub = strtok(rem, "/");

	while (sub) {
		if (strlen(tmp) + strlen(sub) + 2 > PATH_MAX)
			return -EINVAL;

		strcat(tmp, sub);
		strcat(tmp, "/");

		ret = mkdir(tmp, S_IRWXU);
		if (ret && errno != EEXIST) {
			fprintf(stderr, "mkdir %s failed: %s\n", tmp,
				strerror(errno));
			return ret;
		}

		sub = strtok(NULL, "/");
	}

	return 0;
}

static int bpf_place_pinned(int fd, const char *name,
			    const struct bpf_elf_ctx *ctx, uint32_t pinning)
{
	char pathname[PATH_MAX];
	const char *tmp;
	int ret = 0;

	if (bpf_no_pinning(ctx, pinning) || !bpf_get_work_dir(ctx->type))
		return 0;

	if (pinning == PIN_OBJECT_NS)
		ret = bpf_make_obj_path(ctx);
	else if ((tmp = bpf_custom_pinning(ctx, pinning)))
		ret = bpf_make_custom_path(ctx, tmp);
	if (ret < 0)
		return ret;

	bpf_make_pathname(pathname, sizeof(pathname), name, ctx, pinning);
	return bpf_obj_pin(fd, pathname);
}

static void bpf_prog_report(int fd, const char *section,
			    const struct bpf_elf_prog *prog,
			    struct bpf_elf_ctx *ctx)
{
	unsigned int insns = prog->size / sizeof(struct bpf_insn);

	fprintf(stderr, "\nProg section \'%s\' %s%s (%d)!\n", section,
		fd < 0 ? "rejected: " : "loaded",
		fd < 0 ? strerror(errno) : "",
		fd < 0 ? errno : fd);

	fprintf(stderr, " - Type:         %u\n", prog->type);
	fprintf(stderr, " - Instructions: %u (%u over limit)\n",
		insns, insns > BPF_MAXINSNS ? insns - BPF_MAXINSNS : 0);
	fprintf(stderr, " - License:      %s\n\n", prog->license);

	bpf_dump_error(ctx, "Verifier analysis:\n\n");
}

static int bpf_prog_attach(const char *section,
			   const struct bpf_elf_prog *prog,
			   struct bpf_elf_ctx *ctx)
{
	int tries = 0, fd;
retry:
	errno = 0;
	fd = bpf_prog_load(prog->type, prog->insns, prog->size,
			   prog->license, ctx->log, ctx->log_size);
	if (fd < 0 || ctx->verbose) {
		/* The verifier log is pretty chatty, sometimes so chatty
		 * on larger programs, that we could fail to dump everything
		 * into our buffer. Still, try to give a debuggable error
		 * log for the user, so enlarge it and re-fail.
		 */
		if (fd < 0 && (errno == ENOSPC || !ctx->log_size)) {
			if (tries++ < 10 && !bpf_log_realloc(ctx))
				goto retry;

			fprintf(stderr, "Log buffer too small to dump verifier log %zu bytes (%d tries)!\n",
				ctx->log_size, tries);
			return fd;
		}

		bpf_prog_report(fd, section, prog, ctx);
	}

	return fd;
}

static void bpf_map_report(int fd, const char *name,
			   const struct bpf_elf_map *map,
			   struct bpf_elf_ctx *ctx, int inner_fd)
{
	fprintf(stderr, "Map object \'%s\' %s%s (%d)!\n", name,
		fd < 0 ? "rejected: " : "loaded",
		fd < 0 ? strerror(errno) : "",
		fd < 0 ? errno : fd);

	fprintf(stderr, " - Type:         %u\n", map->type);
	fprintf(stderr, " - Identifier:   %u\n", map->id);
	fprintf(stderr, " - Pinning:      %u\n", map->pinning);
	fprintf(stderr, " - Size key:     %u\n", map->size_key);
	fprintf(stderr, " - Size value:   %u\n",
		inner_fd ? (int)sizeof(int) : map->size_value);
	fprintf(stderr, " - Max elems:    %u\n", map->max_elem);
	fprintf(stderr, " - Flags:        %#x\n\n", map->flags);
}

static int bpf_find_map_id(const struct bpf_elf_ctx *ctx, uint32_t id)
{
	int i;

	for (i = 0; i < ctx->map_num; i++) {
		if (ctx->maps[i].id != id)
			continue;
		if (ctx->map_fds[i] < 0)
			return -EINVAL;

		return ctx->map_fds[i];
	}

	return -ENOENT;
}

static void bpf_report_map_in_map(int outer_fd, uint32_t idx)
{
	struct bpf_elf_map outer_map;
	int ret;

	fprintf(stderr, "Cannot insert map into map! ");

	ret = bpf_derive_elf_map_from_fdinfo(outer_fd, &outer_map, NULL);
	if (!ret) {
		if (idx >= outer_map.max_elem &&
		    outer_map.type == BPF_MAP_TYPE_ARRAY_OF_MAPS) {
			fprintf(stderr, "Outer map has %u elements, index %u is invalid!\n",
				outer_map.max_elem, idx);
			return;
		}
	}

	fprintf(stderr, "Different map specs used for outer and inner map?\n");
}

static bool bpf_is_map_in_map_type(const struct bpf_elf_map *map)
{
	return map->type == BPF_MAP_TYPE_ARRAY_OF_MAPS ||
	       map->type == BPF_MAP_TYPE_HASH_OF_MAPS;
}

static int bpf_map_attach(const char *name, struct bpf_elf_ctx *ctx,
			  const struct bpf_elf_map *map, struct bpf_map_ext *ext,
			  int *have_map_in_map)
{
	int fd, ret, map_inner_fd = 0;

	fd = bpf_probe_pinned(name, ctx, map->pinning);
	if (fd > 0) {
		ret = bpf_map_selfcheck_pinned(fd, map, ext,
					       offsetof(struct bpf_elf_map,
							id), ctx->type);
		if (ret < 0) {
			close(fd);
			fprintf(stderr, "Map \'%s\' self-check failed!\n",
				name);
			return ret;
		}
		if (ctx->verbose)
			fprintf(stderr, "Map \'%s\' loaded as pinned!\n",
				name);
		return fd;
	}

	if (have_map_in_map && bpf_is_map_in_map_type(map)) {
		(*have_map_in_map)++;
		if (map->inner_id)
			return 0;
		fprintf(stderr, "Map \'%s\' cannot be created since no inner map ID defined!\n",
			name);
		return -EINVAL;
	}

	if (!have_map_in_map && bpf_is_map_in_map_type(map)) {
		map_inner_fd = bpf_find_map_id(ctx, map->inner_id);
		if (map_inner_fd < 0) {
			fprintf(stderr, "Map \'%s\' cannot be loaded. Inner map with ID %u not found!\n",
				name, map->inner_id);
			return -EINVAL;
		}
	}

	errno = 0;
	fd = bpf_map_create(map->type, map->size_key, map->size_value,
			    map->max_elem, map->flags, map_inner_fd);
	if (fd < 0 || ctx->verbose) {
		bpf_map_report(fd, name, map, ctx, map_inner_fd);
		if (fd < 0)
			return fd;
	}

	ret = bpf_place_pinned(fd, name, ctx, map->pinning);
	if (ret < 0 && errno != EEXIST) {
		fprintf(stderr, "Could not pin %s map: %s\n", name,
			strerror(errno));
		close(fd);
		return ret;
	}

	return fd;
}

static const char *bpf_str_tab_name(const struct bpf_elf_ctx *ctx,
				    const GElf_Sym *sym)
{
	return ctx->str_tab->d_buf + sym->st_name;
}

static const char *bpf_map_fetch_name(struct bpf_elf_ctx *ctx, int which)
{
	GElf_Sym sym;
	int i;

	for (i = 0; i < ctx->sym_num; i++) {
		if (gelf_getsym(ctx->sym_tab, i, &sym) != &sym)
			continue;

		if (GELF_ST_BIND(sym.st_info) != STB_GLOBAL ||
		    GELF_ST_TYPE(sym.st_info) != STT_NOTYPE ||
		    sym.st_shndx != ctx->sec_maps ||
		    sym.st_value / ctx->map_len != which)
			continue;

		return bpf_str_tab_name(ctx, &sym);
	}

	return NULL;
}

static int bpf_maps_attach_all(struct bpf_elf_ctx *ctx)
{
	int i, j, ret, fd, inner_fd, inner_idx, have_map_in_map = 0;
	const char *map_name;

	for (i = 0; i < ctx->map_num; i++) {
		map_name = bpf_map_fetch_name(ctx, i);
		if (!map_name)
			return -EIO;

		fd = bpf_map_attach(map_name, ctx, &ctx->maps[i],
				    &ctx->maps_ext[i], &have_map_in_map);
		if (fd < 0)
			return fd;

		ctx->map_fds[i] = !fd ? -1 : fd;
	}

	for (i = 0; have_map_in_map && i < ctx->map_num; i++) {
		if (ctx->map_fds[i] >= 0)
			continue;

		map_name = bpf_map_fetch_name(ctx, i);
		if (!map_name)
			return -EIO;

		fd = bpf_map_attach(map_name, ctx, &ctx->maps[i],
				    &ctx->maps_ext[i], NULL);
		if (fd < 0)
			return fd;

		ctx->map_fds[i] = fd;
	}

	for (i = 0; have_map_in_map && i < ctx->map_num; i++) {
		if (!ctx->maps[i].id ||
		    ctx->maps[i].inner_id ||
		    ctx->maps[i].inner_idx == -1)
			continue;

		inner_fd  = ctx->map_fds[i];
		inner_idx = ctx->maps[i].inner_idx;

		for (j = 0; j < ctx->map_num; j++) {
			if (!bpf_is_map_in_map_type(&ctx->maps[j]))
				continue;
			if (ctx->maps[j].inner_id != ctx->maps[i].id)
				continue;

			ret = bpf_map_update(ctx->map_fds[j], &inner_idx,
					     &inner_fd, BPF_ANY);
			if (ret < 0) {
				bpf_report_map_in_map(ctx->map_fds[j],
						      inner_idx);
				return ret;
			}
		}
	}

	return 0;
}

static int bpf_map_num_sym(struct bpf_elf_ctx *ctx)
{
	int i, num = 0;
	GElf_Sym sym;

	for (i = 0; i < ctx->sym_num; i++) {
		if (gelf_getsym(ctx->sym_tab, i, &sym) != &sym)
			continue;

		if (GELF_ST_BIND(sym.st_info) != STB_GLOBAL ||
		    GELF_ST_TYPE(sym.st_info) != STT_NOTYPE ||
		    sym.st_shndx != ctx->sec_maps)
			continue;
		num++;
	}

	return num;
}

static int bpf_fill_section_data(struct bpf_elf_ctx *ctx, int section,
				 struct bpf_elf_sec_data *data)
{
	Elf_Data *sec_edata;
	GElf_Shdr sec_hdr;
	Elf_Scn *sec_fd;
	char *sec_name;

	memset(data, 0, sizeof(*data));

	sec_fd = elf_getscn(ctx->elf_fd, section);
	if (!sec_fd)
		return -EINVAL;
	if (gelf_getshdr(sec_fd, &sec_hdr) != &sec_hdr)
		return -EIO;

	sec_name = elf_strptr(ctx->elf_fd, ctx->elf_hdr.e_shstrndx,
			      sec_hdr.sh_name);
	if (!sec_name || !sec_hdr.sh_size)
		return -ENOENT;

	sec_edata = elf_getdata(sec_fd, NULL);
	if (!sec_edata || elf_getdata(sec_fd, sec_edata))
		return -EIO;

	memcpy(&data->sec_hdr, &sec_hdr, sizeof(sec_hdr));

	data->sec_name = sec_name;
	data->sec_data = sec_edata;
	return 0;
}

struct bpf_elf_map_min {
	__u32 type;
	__u32 size_key;
	__u32 size_value;
	__u32 max_elem;
};

static int bpf_fetch_maps_begin(struct bpf_elf_ctx *ctx, int section,
				struct bpf_elf_sec_data *data)
{
	ctx->map_num = data->sec_data->d_size;
	ctx->sec_maps = section;
	ctx->sec_done[section] = true;

	if (ctx->map_num > sizeof(ctx->maps)) {
		fprintf(stderr, "Too many BPF maps in ELF section!\n");
		return -ENOMEM;
	}

	memcpy(ctx->maps, data->sec_data->d_buf, ctx->map_num);
	return 0;
}

static int bpf_map_verify_all_offs(struct bpf_elf_ctx *ctx, int end)
{
	GElf_Sym sym;
	int off, i;

	for (off = 0; off < end; off += ctx->map_len) {
		/* Order doesn't need to be linear here, hence we walk
		 * the table again.
		 */
		for (i = 0; i < ctx->sym_num; i++) {
			if (gelf_getsym(ctx->sym_tab, i, &sym) != &sym)
				continue;
			if (GELF_ST_BIND(sym.st_info) != STB_GLOBAL ||
			    GELF_ST_TYPE(sym.st_info) != STT_NOTYPE ||
			    sym.st_shndx != ctx->sec_maps)
				continue;
			if (sym.st_value == off)
				break;
			if (i == ctx->sym_num - 1)
				return -1;
		}
	}

	return off == end ? 0 : -1;
}

static int bpf_fetch_maps_end(struct bpf_elf_ctx *ctx)
{
	struct bpf_elf_map fixup[ARRAY_SIZE(ctx->maps)] = {};
	int i, sym_num = bpf_map_num_sym(ctx);
	__u8 *buff;

	if (sym_num == 0 || sym_num > ARRAY_SIZE(ctx->maps)) {
		fprintf(stderr, "%u maps not supported in current map section!\n",
			sym_num);
		return -EINVAL;
	}

	if (ctx->map_num % sym_num != 0 ||
	    ctx->map_num % sizeof(__u32) != 0) {
		fprintf(stderr, "Number BPF map symbols are not multiple of struct bpf_elf_map!\n");
		return -EINVAL;
	}

	ctx->map_len = ctx->map_num / sym_num;
	if (bpf_map_verify_all_offs(ctx, ctx->map_num)) {
		fprintf(stderr, "Different struct bpf_elf_map in use!\n");
		return -EINVAL;
	}

	if (ctx->map_len == sizeof(struct bpf_elf_map)) {
		ctx->map_num = sym_num;
		return 0;
	} else if (ctx->map_len > sizeof(struct bpf_elf_map)) {
		fprintf(stderr, "struct bpf_elf_map not supported, coming from future version?\n");
		return -EINVAL;
	} else if (ctx->map_len < sizeof(struct bpf_elf_map_min)) {
		fprintf(stderr, "struct bpf_elf_map too small, not supported!\n");
		return -EINVAL;
	}

	ctx->map_num = sym_num;
	for (i = 0, buff = (void *)ctx->maps; i < ctx->map_num;
	     i++, buff += ctx->map_len) {
		/* The fixup leaves the rest of the members as zero, which
		 * is fine currently, but option exist to set some other
		 * default value as well when needed in future.
		 */
		memcpy(&fixup[i], buff, ctx->map_len);
	}

	memcpy(ctx->maps, fixup, sizeof(fixup));

	printf("Note: %zu bytes struct bpf_elf_map fixup performed due to size mismatch!\n",
	       sizeof(struct bpf_elf_map) - ctx->map_len);
	return 0;
}

static int bpf_fetch_license(struct bpf_elf_ctx *ctx, int section,
			     struct bpf_elf_sec_data *data)
{
	if (data->sec_data->d_size > sizeof(ctx->license))
		return -ENOMEM;

	memcpy(ctx->license, data->sec_data->d_buf, data->sec_data->d_size);
	ctx->sec_done[section] = true;
	return 0;
}

static int bpf_fetch_symtab(struct bpf_elf_ctx *ctx, int section,
			    struct bpf_elf_sec_data *data)
{
	ctx->sym_tab = data->sec_data;
	ctx->sym_num = data->sec_hdr.sh_size / data->sec_hdr.sh_entsize;
	ctx->sec_done[section] = true;
	return 0;
}

static int bpf_fetch_strtab(struct bpf_elf_ctx *ctx, int section,
			    struct bpf_elf_sec_data *data)
{
	ctx->str_tab = data->sec_data;
	ctx->sec_done[section] = true;
	return 0;
}

static bool bpf_has_map_data(const struct bpf_elf_ctx *ctx)
{
	return ctx->sym_tab && ctx->str_tab && ctx->sec_maps;
}

static int bpf_fetch_ancillary(struct bpf_elf_ctx *ctx)
{
	struct bpf_elf_sec_data data;
	int i, ret = -1;

	for (i = 1; i < ctx->elf_hdr.e_shnum; i++) {
		ret = bpf_fill_section_data(ctx, i, &data);
		if (ret < 0)
			continue;

		if (data.sec_hdr.sh_type == SHT_PROGBITS &&
		    !strcmp(data.sec_name, ELF_SECTION_MAPS))
			ret = bpf_fetch_maps_begin(ctx, i, &data);
		else if (data.sec_hdr.sh_type == SHT_PROGBITS &&
			 !strcmp(data.sec_name, ELF_SECTION_LICENSE))
			ret = bpf_fetch_license(ctx, i, &data);
		else if (data.sec_hdr.sh_type == SHT_SYMTAB &&
			 !strcmp(data.sec_name, ".symtab"))
			ret = bpf_fetch_symtab(ctx, i, &data);
		else if (data.sec_hdr.sh_type == SHT_STRTAB &&
			 !strcmp(data.sec_name, ".strtab"))
			ret = bpf_fetch_strtab(ctx, i, &data);
		if (ret < 0) {
			fprintf(stderr, "Error parsing section %d! Perhaps check with readelf -a?\n",
				i);
			return ret;
		}
	}

	if (bpf_has_map_data(ctx)) {
		ret = bpf_fetch_maps_end(ctx);
		if (ret < 0) {
			fprintf(stderr, "Error fixing up map structure, incompatible struct bpf_elf_map used?\n");
			return ret;
		}

		ret = bpf_maps_attach_all(ctx);
		if (ret < 0) {
			fprintf(stderr, "Error loading maps into kernel!\n");
			return ret;
		}
	}

	return ret;
}

static int bpf_fetch_prog(struct bpf_elf_ctx *ctx, const char *section,
			  bool *sseen)
{
	struct bpf_elf_sec_data data;
	struct bpf_elf_prog prog;
	int ret, i, fd = -1;

	for (i = 1; i < ctx->elf_hdr.e_shnum; i++) {
		if (ctx->sec_done[i])
			continue;

		ret = bpf_fill_section_data(ctx, i, &data);
		if (ret < 0 ||
		    !(data.sec_hdr.sh_type == SHT_PROGBITS &&
		      data.sec_hdr.sh_flags & SHF_EXECINSTR &&
		      !strcmp(data.sec_name, section)))
			continue;

		*sseen = true;

		memset(&prog, 0, sizeof(prog));
		prog.type    = ctx->type;
		prog.insns   = data.sec_data->d_buf;
		prog.size    = data.sec_data->d_size;
		prog.license = ctx->license;

		fd = bpf_prog_attach(section, &prog, ctx);
		if (fd < 0)
			return fd;

		ctx->sec_done[i] = true;
		break;
	}

	return fd;
}

struct bpf_tail_call_props {
	unsigned int total;
	unsigned int jited;
};

static int bpf_apply_relo_data(struct bpf_elf_ctx *ctx,
			       struct bpf_elf_sec_data *data_relo,
			       struct bpf_elf_sec_data *data_insn,
			       struct bpf_tail_call_props *props)
{
	Elf_Data *idata = data_insn->sec_data;
	GElf_Shdr *rhdr = &data_relo->sec_hdr;
	int relo_ent, relo_num = rhdr->sh_size / rhdr->sh_entsize;
	struct bpf_insn *insns = idata->d_buf;
	unsigned int num_insns = idata->d_size / sizeof(*insns);

	for (relo_ent = 0; relo_ent < relo_num; relo_ent++) {
		unsigned int ioff, rmap;
		GElf_Rel relo;
		GElf_Sym sym;

		if (gelf_getrel(data_relo->sec_data, relo_ent, &relo) != &relo)
			return -EIO;

		ioff = relo.r_offset / sizeof(struct bpf_insn);
		if (ioff >= num_insns ||
		    insns[ioff].code != (BPF_LD | BPF_IMM | BPF_DW)) {
			fprintf(stderr, "ELF contains relo data for non ld64 instruction at offset %u! Compiler bug?!\n",
				ioff);
			if (ioff < num_insns &&
			    insns[ioff].code == (BPF_JMP | BPF_CALL))
				fprintf(stderr, " - Try to annotate functions with always_inline attribute!\n");
			return -EINVAL;
		}

		if (gelf_getsym(ctx->sym_tab, GELF_R_SYM(relo.r_info), &sym) != &sym)
			return -EIO;
		if (sym.st_shndx != ctx->sec_maps) {
			fprintf(stderr, "ELF contains non-map related relo data in entry %u pointing to section %u! Compiler bug?!\n",
				relo_ent, sym.st_shndx);
			return -EIO;
		}

		rmap = sym.st_value / ctx->map_len;
		if (rmap >= ARRAY_SIZE(ctx->map_fds))
			return -EINVAL;
		if (!ctx->map_fds[rmap])
			return -EINVAL;
		if (ctx->maps[rmap].type == BPF_MAP_TYPE_PROG_ARRAY) {
			props->total++;
			if (ctx->maps_ext[rmap].owner.jited ||
			    (ctx->maps_ext[rmap].owner.type == 0 &&
			     ctx->cfg.jit_enabled))
				props->jited++;
		}

		if (ctx->verbose)
			fprintf(stderr, "Map \'%s\' (%d) injected into prog section \'%s\' at offset %u!\n",
				bpf_str_tab_name(ctx, &sym), ctx->map_fds[rmap],
				data_insn->sec_name, ioff);

		insns[ioff].src_reg = BPF_PSEUDO_MAP_FD;
		insns[ioff].imm     = ctx->map_fds[rmap];
	}

	return 0;
}

static int bpf_fetch_prog_relo(struct bpf_elf_ctx *ctx, const char *section,
			       bool *lderr, bool *sseen)
{
	struct bpf_elf_sec_data data_relo, data_insn;
	struct bpf_elf_prog prog;
	int ret, idx, i, fd = -1;

	for (i = 1; i < ctx->elf_hdr.e_shnum; i++) {
		struct bpf_tail_call_props props = {};

		ret = bpf_fill_section_data(ctx, i, &data_relo);
		if (ret < 0 || data_relo.sec_hdr.sh_type != SHT_REL)
			continue;

		idx = data_relo.sec_hdr.sh_info;

		ret = bpf_fill_section_data(ctx, idx, &data_insn);
		if (ret < 0 ||
		    !(data_insn.sec_hdr.sh_type == SHT_PROGBITS &&
		      data_insn.sec_hdr.sh_flags & SHF_EXECINSTR &&
		      !strcmp(data_insn.sec_name, section)))
			continue;

		*sseen = true;

		ret = bpf_apply_relo_data(ctx, &data_relo, &data_insn, &props);
		if (ret < 0) {
			*lderr = true;
			return ret;
		}

		memset(&prog, 0, sizeof(prog));
		prog.type    = ctx->type;
		prog.insns   = data_insn.sec_data->d_buf;
		prog.size    = data_insn.sec_data->d_size;
		prog.license = ctx->license;

		fd = bpf_prog_attach(section, &prog, ctx);
		if (fd < 0) {
			*lderr = true;
			if (props.total) {
				if (ctx->cfg.jit_enabled &&
				    props.total != props.jited)
					fprintf(stderr, "JIT enabled, but only %u/%u tail call maps in the program have JITed owner!\n",
						props.jited, props.total);
				if (!ctx->cfg.jit_enabled &&
				    props.jited)
					fprintf(stderr, "JIT disabled, but %u/%u tail call maps in the program have JITed owner!\n",
						props.jited, props.total);
			}
			return fd;
		}

		ctx->sec_done[i]   = true;
		ctx->sec_done[idx] = true;
		break;
	}

	return fd;
}

static int bpf_fetch_prog_sec(struct bpf_elf_ctx *ctx, const char *section)
{
	bool lderr = false, sseen = false;
	int ret = -1;

	if (bpf_has_map_data(ctx))
		ret = bpf_fetch_prog_relo(ctx, section, &lderr, &sseen);
	if (ret < 0 && !lderr)
		ret = bpf_fetch_prog(ctx, section, &sseen);
	if (ret < 0 && !sseen)
		fprintf(stderr, "Program section \'%s\' not found in ELF file!\n",
			section);
	return ret;
}

static int bpf_find_map_by_id(struct bpf_elf_ctx *ctx, uint32_t id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ctx->map_fds); i++)
		if (ctx->map_fds[i] && ctx->maps[i].id == id &&
		    ctx->maps[i].type == BPF_MAP_TYPE_PROG_ARRAY)
			return i;
	return -1;
}

struct bpf_jited_aux {
	int prog_fd;
	int map_fd;
	struct bpf_prog_data prog;
	struct bpf_map_ext map;
};

static int bpf_derive_prog_from_fdinfo(int fd, struct bpf_prog_data *prog)
{
	char file[PATH_MAX], buff[4096];
	unsigned int val;
	FILE *fp;

	snprintf(file, sizeof(file), "/proc/%d/fdinfo/%d", getpid(), fd);
	memset(prog, 0, sizeof(*prog));

	fp = fopen(file, "r");
	if (!fp) {
		fprintf(stderr, "No procfs support?!\n");
		return -EIO;
	}

	while (fgets(buff, sizeof(buff), fp)) {
		if (sscanf(buff, "prog_type:\t%u", &val) == 1)
			prog->type = val;
		else if (sscanf(buff, "prog_jited:\t%u", &val) == 1)
			prog->jited = val;
	}

	fclose(fp);
	return 0;
}

static int bpf_tail_call_get_aux(struct bpf_jited_aux *aux)
{
	struct bpf_elf_map tmp;
	int ret;

	ret = bpf_derive_elf_map_from_fdinfo(aux->map_fd, &tmp, &aux->map);
	if (!ret)
		ret = bpf_derive_prog_from_fdinfo(aux->prog_fd, &aux->prog);

	return ret;
}

static int bpf_fill_prog_arrays(struct bpf_elf_ctx *ctx)
{
	struct bpf_elf_sec_data data;
	uint32_t map_id, key_id;
	int fd, i, ret, idx;

	for (i = 1; i < ctx->elf_hdr.e_shnum; i++) {
		if (ctx->sec_done[i])
			continue;

		ret = bpf_fill_section_data(ctx, i, &data);
		if (ret < 0)
			continue;

		ret = sscanf(data.sec_name, "%i/%i", &map_id, &key_id);
		if (ret != 2)
			continue;

		idx = bpf_find_map_by_id(ctx, map_id);
		if (idx < 0)
			continue;

		fd = bpf_fetch_prog_sec(ctx, data.sec_name);
		if (fd < 0)
			return -EIO;

		ret = bpf_map_update(ctx->map_fds[idx], &key_id,
				     &fd, BPF_ANY);
		if (ret < 0) {
			struct bpf_jited_aux aux = {};

			ret = -errno;
			if (errno == E2BIG) {
				fprintf(stderr, "Tail call key %u for map %u out of bounds?\n",
					key_id, map_id);
				return ret;
			}

			aux.map_fd  = ctx->map_fds[idx];
			aux.prog_fd = fd;

			if (bpf_tail_call_get_aux(&aux))
				return ret;
			if (!aux.map.owner.type)
				return ret;

			if (aux.prog.type != aux.map.owner.type)
				fprintf(stderr, "Tail call map owned by prog type %u, but prog type is %u!\n",
					aux.map.owner.type, aux.prog.type);
			if (aux.prog.jited != aux.map.owner.jited)
				fprintf(stderr, "Tail call map %s jited, but prog %s!\n",
					aux.map.owner.jited ? "is" : "not",
					aux.prog.jited ? "is" : "not");
			return ret;
		}

		ctx->sec_done[i] = true;
	}

	return 0;
}

static void bpf_save_finfo(struct bpf_elf_ctx *ctx)
{
	struct stat st;
	int ret;

	memset(&ctx->stat, 0, sizeof(ctx->stat));

	ret = fstat(ctx->obj_fd, &st);
	if (ret < 0) {
		fprintf(stderr, "Stat of elf file failed: %s\n",
			strerror(errno));
		return;
	}

	ctx->stat.st_dev = st.st_dev;
	ctx->stat.st_ino = st.st_ino;
}

static int bpf_read_pin_mapping(FILE *fp, uint32_t *id, char *path)
{
	char buff[PATH_MAX];

	while (fgets(buff, sizeof(buff), fp)) {
		char *ptr = buff;

		while (*ptr == ' ' || *ptr == '\t')
			ptr++;

		if (*ptr == '#' || *ptr == '\n' || *ptr == 0)
			continue;

		if (sscanf(ptr, "%i %s\n", id, path) != 2 &&
		    sscanf(ptr, "%i %s #", id, path) != 2) {
			strcpy(path, ptr);
			return -1;
		}

		return 1;
	}

	return 0;
}

static bool bpf_pinning_reserved(uint32_t pinning)
{
	switch (pinning) {
	case PIN_NONE:
	case PIN_OBJECT_NS:
	case PIN_GLOBAL_NS:
		return true;
	default:
		return false;
	}
}

static void bpf_hash_init(struct bpf_elf_ctx *ctx, const char *db_file)
{
	struct bpf_hash_entry *entry;
	char subpath[PATH_MAX] = {};
	uint32_t pinning;
	FILE *fp;
	int ret;

	fp = fopen(db_file, "r");
	if (!fp)
		return;

	while ((ret = bpf_read_pin_mapping(fp, &pinning, subpath))) {
		if (ret == -1) {
			fprintf(stderr, "Database %s is corrupted at: %s\n",
				db_file, subpath);
			fclose(fp);
			return;
		}

		if (bpf_pinning_reserved(pinning)) {
			fprintf(stderr, "Database %s, id %u is reserved - ignoring!\n",
				db_file, pinning);
			continue;
		}

		entry = malloc(sizeof(*entry));
		if (!entry) {
			fprintf(stderr, "No memory left for db entry!\n");
			continue;
		}

		entry->pinning = pinning;
		entry->subpath = strdup(subpath);
		if (!entry->subpath) {
			fprintf(stderr, "No memory left for db entry!\n");
			free(entry);
			continue;
		}

		entry->next = ctx->ht[pinning & (ARRAY_SIZE(ctx->ht) - 1)];
		ctx->ht[pinning & (ARRAY_SIZE(ctx->ht) - 1)] = entry;
	}

	fclose(fp);
}

static void bpf_hash_destroy(struct bpf_elf_ctx *ctx)
{
	struct bpf_hash_entry *entry;
	int i;

	for (i = 0; i < ARRAY_SIZE(ctx->ht); i++) {
		while ((entry = ctx->ht[i]) != NULL) {
			ctx->ht[i] = entry->next;
			free((char *)entry->subpath);
			free(entry);
		}
	}
}

static int bpf_elf_check_ehdr(const struct bpf_elf_ctx *ctx)
{
	if (ctx->elf_hdr.e_type != ET_REL ||
	    (ctx->elf_hdr.e_machine != EM_NONE &&
	     ctx->elf_hdr.e_machine != EM_BPF) ||
	    ctx->elf_hdr.e_version != EV_CURRENT) {
		fprintf(stderr, "ELF format error, ELF file not for eBPF?\n");
		return -EINVAL;
	}

	switch (ctx->elf_hdr.e_ident[EI_DATA]) {
	default:
		fprintf(stderr, "ELF format error, wrong endianness info?\n");
		return -EINVAL;
	case ELFDATA2LSB:
		if (htons(1) == 1) {
			fprintf(stderr,
				"We are big endian, eBPF object is little endian!\n");
			return -EIO;
		}
		break;
	case ELFDATA2MSB:
		if (htons(1) != 1) {
			fprintf(stderr,
				"We are little endian, eBPF object is big endian!\n");
			return -EIO;
		}
		break;
	}

	return 0;
}

static void bpf_get_cfg(struct bpf_elf_ctx *ctx)
{
	static const char *path_jit = "/proc/sys/net/core/bpf_jit_enable";
	int fd;

	fd = open(path_jit, O_RDONLY);
	if (fd > 0) {
		char tmp[16] = {};

		if (read(fd, tmp, sizeof(tmp)) > 0)
			ctx->cfg.jit_enabled = atoi(tmp);
		close(fd);
	}
}

static int bpf_elf_ctx_init(struct bpf_elf_ctx *ctx, const char *pathname,
			    enum bpf_prog_type type, bool verbose)
{
	int ret = -EINVAL;

	if (elf_version(EV_CURRENT) == EV_NONE ||
	    bpf_init_env(pathname))
		return ret;

	memset(ctx, 0, sizeof(*ctx));
	bpf_get_cfg(ctx);
	ctx->verbose = verbose;
	ctx->type    = type;

	ctx->obj_fd = open(pathname, O_RDONLY);
	if (ctx->obj_fd < 0)
		return ctx->obj_fd;

	ctx->elf_fd = elf_begin(ctx->obj_fd, ELF_C_READ, NULL);
	if (!ctx->elf_fd) {
		ret = -EINVAL;
		goto out_fd;
	}

	if (elf_kind(ctx->elf_fd) != ELF_K_ELF) {
		ret = -EINVAL;
		goto out_fd;
	}

	if (gelf_getehdr(ctx->elf_fd, &ctx->elf_hdr) !=
	    &ctx->elf_hdr) {
		ret = -EIO;
		goto out_elf;
	}

	ret = bpf_elf_check_ehdr(ctx);
	if (ret < 0)
		goto out_elf;

	ctx->sec_done = calloc(ctx->elf_hdr.e_shnum,
			       sizeof(*(ctx->sec_done)));
	if (!ctx->sec_done) {
		ret = -ENOMEM;
		goto out_elf;
	}

	if (ctx->verbose && bpf_log_realloc(ctx)) {
		ret = -ENOMEM;
		goto out_free;
	}

	bpf_save_finfo(ctx);
	bpf_hash_init(ctx, CONFDIR "/bpf_pinning");

	return 0;
out_free:
	free(ctx->sec_done);
out_elf:
	elf_end(ctx->elf_fd);
out_fd:
	close(ctx->obj_fd);
	return ret;
}

static int bpf_maps_count(struct bpf_elf_ctx *ctx)
{
	int i, count = 0;

	for (i = 0; i < ARRAY_SIZE(ctx->map_fds); i++) {
		if (!ctx->map_fds[i])
			break;
		count++;
	}

	return count;
}

static void bpf_maps_teardown(struct bpf_elf_ctx *ctx)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ctx->map_fds); i++) {
		if (ctx->map_fds[i])
			close(ctx->map_fds[i]);
	}
}

static void bpf_elf_ctx_destroy(struct bpf_elf_ctx *ctx, bool failure)
{
	if (failure)
		bpf_maps_teardown(ctx);

	bpf_hash_destroy(ctx);

	free(ctx->sec_done);
	free(ctx->log);

	elf_end(ctx->elf_fd);
	close(ctx->obj_fd);
}

static struct bpf_elf_ctx __ctx;

static int bpf_obj_open(const char *pathname, enum bpf_prog_type type,
			const char *section, bool verbose)
{
	struct bpf_elf_ctx *ctx = &__ctx;
	int fd = 0, ret;

	ret = bpf_elf_ctx_init(ctx, pathname, type, verbose);
	if (ret < 0) {
		fprintf(stderr, "Cannot initialize ELF context!\n");
		return ret;
	}

	ret = bpf_fetch_ancillary(ctx);
	if (ret < 0) {
		fprintf(stderr, "Error fetching ELF ancillary data!\n");
		goto out;
	}

	fd = bpf_fetch_prog_sec(ctx, section);
	if (fd < 0) {
		fprintf(stderr, "Error fetching program/map!\n");
		ret = fd;
		goto out;
	}

	ret = bpf_fill_prog_arrays(ctx);
	if (ret < 0)
		fprintf(stderr, "Error filling program arrays!\n");
out:
	bpf_elf_ctx_destroy(ctx, ret < 0);
	if (ret < 0) {
		if (fd)
			close(fd);
		return ret;
	}

	return fd;
}

static int
bpf_map_set_send(int fd, struct sockaddr_un *addr, unsigned int addr_len,
		 const struct bpf_map_data *aux, unsigned int entries)
{
	struct bpf_map_set_msg msg = {
		.aux.uds_ver = BPF_SCM_AUX_VER,
		.aux.num_ent = entries,
	};
	int *cmsg_buf, min_fd;
	char *amsg_buf;
	int i;

	strncpy(msg.aux.obj_name, aux->obj, sizeof(msg.aux.obj_name));
	memcpy(&msg.aux.obj_st, aux->st, sizeof(msg.aux.obj_st));

	cmsg_buf = bpf_map_set_init(&msg, addr, addr_len);
	amsg_buf = (char *)msg.aux.ent;

	for (i = 0; i < entries; i += min_fd) {
		int ret;

		min_fd = min(BPF_SCM_MAX_FDS * 1U, entries - i);
		bpf_map_set_init_single(&msg, min_fd);

		memcpy(cmsg_buf, &aux->fds[i], sizeof(aux->fds[0]) * min_fd);
		memcpy(amsg_buf, &aux->ent[i], sizeof(aux->ent[0]) * min_fd);

		ret = sendmsg(fd, &msg.hdr, 0);
		if (ret <= 0)
			return ret ? : -1;
	}

	return 0;
}

static int
bpf_map_set_recv(int fd, int *fds,  struct bpf_map_aux *aux,
		 unsigned int entries)
{
	struct bpf_map_set_msg msg;
	int *cmsg_buf, min_fd;
	char *amsg_buf, *mmsg_buf;
	unsigned int needed = 1;
	int i;

	cmsg_buf = bpf_map_set_init(&msg, NULL, 0);
	amsg_buf = (char *)msg.aux.ent;
	mmsg_buf = (char *)&msg.aux;

	for (i = 0; i < min(entries, needed); i += min_fd) {
		struct cmsghdr *cmsg;
		int ret;

		min_fd = min(entries, entries - i);
		bpf_map_set_init_single(&msg, min_fd);

		ret = recvmsg(fd, &msg.hdr, 0);
		if (ret <= 0)
			return ret ? : -1;

		cmsg = CMSG_FIRSTHDR(&msg.hdr);
		if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS)
			return -EINVAL;
		if (msg.hdr.msg_flags & MSG_CTRUNC)
			return -EIO;
		if (msg.aux.uds_ver != BPF_SCM_AUX_VER)
			return -ENOSYS;

		min_fd = (cmsg->cmsg_len - sizeof(*cmsg)) / sizeof(fd);
		if (min_fd > entries || min_fd <= 0)
			return -EINVAL;

		memcpy(&fds[i], cmsg_buf, sizeof(fds[0]) * min_fd);
		memcpy(&aux->ent[i], amsg_buf, sizeof(aux->ent[0]) * min_fd);
		memcpy(aux, mmsg_buf, offsetof(struct bpf_map_aux, ent));

		needed = aux->num_ent;
	}

	return 0;
}

int bpf_send_map_fds(const char *path, const char *obj)
{
	struct bpf_elf_ctx *ctx = &__ctx;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct bpf_map_data bpf_aux = {
		.fds = ctx->map_fds,
		.ent = ctx->maps,
		.st  = &ctx->stat,
		.obj = obj,
	};
	int fd, ret;

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open socket: %s\n",
			strerror(errno));
		return -1;
	}

	strncpy(addr.sun_path, path, sizeof(addr.sun_path));

	ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		fprintf(stderr, "Cannot connect to %s: %s\n",
			path, strerror(errno));
		return -1;
	}

	ret = bpf_map_set_send(fd, &addr, sizeof(addr), &bpf_aux,
			       bpf_maps_count(ctx));
	if (ret < 0)
		fprintf(stderr, "Cannot send fds to %s: %s\n",
			path, strerror(errno));

	bpf_maps_teardown(ctx);
	close(fd);
	return ret;
}

int bpf_recv_map_fds(const char *path, int *fds, struct bpf_map_aux *aux,
		     unsigned int entries)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd, ret;

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open socket: %s\n",
			strerror(errno));
		return -1;
	}

	strncpy(addr.sun_path, path, sizeof(addr.sun_path));

	ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		fprintf(stderr, "Cannot bind to socket: %s\n",
			strerror(errno));
		return -1;
	}

	ret = bpf_map_set_recv(fd, fds, aux, entries);
	if (ret < 0)
		fprintf(stderr, "Cannot recv fds from %s: %s\n",
			path, strerror(errno));

	unlink(addr.sun_path);
	close(fd);
	return ret;
}
#endif /* HAVE_ELF */
