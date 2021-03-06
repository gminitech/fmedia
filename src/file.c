/** File input.
Copyright (c) 2015 Simon Zolin */

#include <fmedia.h>

#include <FF/sys/fileread.h>
#include <FF/array.h>
#include <FF/time.h>


#undef dbglog
#undef errlog
#undef syserrlog
#define dbglog(trk, ...)  fmed_dbglog(core, trk, "file", __VA_ARGS__)
#define errlog(trk, ...)  fmed_errlog(core, trk, "file", __VA_ARGS__)
#define syserrlog(trk, ...)  fmed_syserrlog(core, trk, "file", __VA_ARGS__)


struct file_in_conf_t {
	uint nbufs;
	size_t bsize;
	size_t align;
	byte directio;
};

typedef struct filemod {
	struct file_in_conf_t in_conf;
} filemod;

static filemod *mod;
const fmed_core *core;

typedef struct fmed_file {
	fffileread *fr;
	const char *fn;

	uint64 fsize;
	int64 seek; //user's read position
	uint nseek;

	fmed_handler handler;
	void *trk;

	unsigned done :1
		, want_read :1;
} fmed_file;

enum {
	FILEIN_MAX_PREBUF = 2, //maximum number of unread buffers
};


//FMEDIA MODULE
static const void* file_iface(const char *name);
static int file_conf(const char *name, ffpars_ctx *ctx);
static int file_sig(uint signo);
static void file_destroy(void);
static const fmed_mod fmed_file_mod = {
	.ver = FMED_VER_FULL, .ver_core = FMED_VER_CORE,
	&file_iface, &file_sig, &file_destroy, &file_conf
};

//INPUT
static void* file_open(fmed_filt *d);
static int file_getdata(void *ctx, fmed_filt *d);
static void file_close(void *ctx);
static int file_in_conf(ffpars_ctx *ctx);
static const fmed_filter fmed_file_input = {
	&file_open, &file_getdata, &file_close
};

static const ffpars_arg file_in_conf_args[] = {
	{ "buffer_size",  FFPARS_TSIZE | FFPARS_FNOTZERO,  FFPARS_DSTOFF(struct file_in_conf_t, bsize) }
	, { "buffers",  FFPARS_TINT | FFPARS_F8BIT,  FFPARS_DSTOFF(struct file_in_conf_t, nbufs) }
	, { "align",  FFPARS_TSIZE | FFPARS_FNOTZERO,  FFPARS_DSTOFF(struct file_in_conf_t, align) }
	, { "direct_io",  FFPARS_TBOOL | FFPARS_F8BIT,  FFPARS_DSTOFF(struct file_in_conf_t, directio) }
};


const fmed_mod* fmed_getmod_file(const fmed_core *_core)
{
	if (mod != NULL)
		return &fmed_file_mod;

	if (0 != ffaio_fctxinit())
		return NULL;
	core = _core;
	if (NULL == (mod = ffmem_tcalloc1(filemod)))
		return NULL;
	return &fmed_file_mod;
}

extern const fmed_filter fmed_file_output;
extern int fileout_config(ffpars_ctx *ctx);
extern int stdout_config(ffpars_ctx *ctx);
extern const fmed_filter file_stdin;
extern const fmed_filter file_stdout;

static const void* file_iface(const char *name)
{
	if (!ffsz_cmp(name, "in")) {
		return &fmed_file_input;
	} else if (!ffsz_cmp(name, "out")) {
		return &fmed_file_output;
	} else if (!ffsz_cmp(name, "stdin"))
		return &file_stdin;
	else if (!ffsz_cmp(name, "stdout"))
		return &file_stdout;
	return NULL;
}

static int file_conf(const char *name, ffpars_ctx *ctx)
{
	if (!ffsz_cmp(name, "in"))
		return file_in_conf(ctx);
	else if (!ffsz_cmp(name, "out"))
		return fileout_config(ctx);
	else if (ffsz_eq(name, "stdout"))
		return stdout_config(ctx);
	return -1;
}

static int file_sig(uint signo)
{
	switch (signo) {
	case FMED_OPEN:
		break;
	}
	return 0;
}

static void file_destroy(void)
{
	ffaio_fctxclose();
	ffmem_free0(mod);
}


static int file_in_conf(ffpars_ctx *ctx)
{
	mod->in_conf.align = 4096;
	mod->in_conf.bsize = 64 * 1024;
	mod->in_conf.nbufs = 3;
	mod->in_conf.directio = 1;
	ffpars_setargs(ctx, &mod->in_conf, file_in_conf_args, FFCNT(file_in_conf_args));
	return 0;
}

static void file_log(void *p, uint level, const ffstr *msg)
{
	fmed_file *f = p;
	switch (level) {
	case 0:
		syserrlog(f->trk, "%S", msg);
		break;
	case 1:
		dbglog(f->trk, "%S", msg);
		break;
	}
}

static void file_onread(void *p)
{
	fmed_file *f = p;
	if (!f->want_read)
		return;
	f->want_read = 0;
	f->handler(f->trk);
}

static void* file_open(fmed_filt *d)
{
	fmed_file *f;
	fffileinfo fi;

	f = ffmem_tcalloc1(fmed_file);
	if (f == NULL)
		return NULL;
	f->fn = d->track->getvalstr(d->trk, "input");
	f->trk = d->trk;

	fffileread_conf conf = {};
	conf.udata = f;
	conf.log = &file_log;
	conf.onread = &file_onread;
	conf.kq = (fffd)d->track->cmd(d->trk, FMED_TRACK_KQ);
	conf.oflags = FFO_RDONLY | FFO_NOATIME | FFO_NONBLOCK | FFO_NODOSNAME;
	conf.bufsize = mod->in_conf.bsize;
	conf.nbufs = mod->in_conf.nbufs;
	conf.bufalign = mod->in_conf.align;
	conf.directio = mod->in_conf.directio;
	f->fr = fffileread_create(f->fn, &conf);
	if (f->fr == NULL) {
		d->e_no_source = (fferr_last() == ENOENT);
		goto done;
	}

	if (0 != fffile_info(fffileread_fd(f->fr), &fi)) {
		syserrlog(d->trk, "%s: %s", fffile_info_S, f->fn);
		goto done;
	}
	f->fsize = fffile_infosize(&fi);

	dbglog(d->trk, "opened %s (%U kbytes)", f->fn, f->fsize / 1024);

	d->input.size = f->fsize;

	if (d->out_preserve_date) {
		d->mtime = fffile_infomtime(&fi);
	}

	f->handler = d->handler;
	return f;

done:
	file_close(f);
	return NULL;
}

static void file_close(void *ctx)
{
	fmed_file *f = ctx;

	if (f->fr != NULL) {
		struct fffileread_stat stat;
		fffileread_stat(f->fr, &stat);
		dbglog(f->trk, "cache-hit#:%u  read#:%u  async#:%u  seek#:%u"
			, stat.ncached, stat.nread, stat.nasync, f->nseek);
		fffileread_unref(f->fr);
	}

	ffmem_free(f);
}

static int file_getdata(void *ctx, fmed_filt *d)
{
	fmed_file *f = ctx;
	ffstr b = {};
	ffbool seek_req = 0;

	if ((int64)d->input.seek != FMED_NULL) {
		f->seek = d->input.seek;
		d->input.seek = FMED_NULL;
		dbglog(d->trk, "seeking to %xU", f->seek);
		f->done = 0;
		seek_req = 1;
		f->nseek++;
	}

	int r = fffileread_getdata(f->fr, &b, f->seek, FFFILEREAD_FREADAHEAD);
	switch ((enum FFFILEREAD_R)r) {

	case FFFILEREAD_RASYNC:
		f->want_read = 1;
		return FMED_RASYNC; //wait until the buffer is full

	case FFFILEREAD_RERR:
		return FMED_RERR;

	case FFFILEREAD_REOF:
		if (f->done || seek_req) {
			/* We finished reading in the previous iteration.
			After that, noone's asked to seek back. */
			d->outlen = 0;
			return FMED_RDONE;
		}
		f->done = 1;
		ffstr_null(&b);
		break;

	case FFFILEREAD_RREAD:
		break;
	}

	d->out = b.ptr,  d->outlen = b.len;
	f->seek += b.len;
	return FMED_ROK;
}
