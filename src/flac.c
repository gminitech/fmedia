/** FLAC input/output.
Copyright (c) 2015 Simon Zolin */

#include <fmedia.h>

#include <FF/audio/flac.h>


static const fmed_core *core;
static uint status;

static const char *const metanames[] = {
	"meta_album"
	, "meta_artist"
	, "meta_comment"
	, "meta_date"
	, "meta_genre"
	, "meta_title"
	, "meta_tracknumber"
};

typedef struct flac {
	ffflac fl;
	char *meta[FFCNT(metanames)];
	uint state;
} flac;

typedef struct flac_out {
	ffflac_enc fl;
} flac_out;

static struct flac_out_conf_t {
	byte level;
	uint min_meta_size;
} flac_out_conf;


//FMEDIA MODULE
static const fmed_filter* flac_iface(const char *name);
static int flac_sig(uint signo);
static void flac_destroy(void);
static const fmed_mod fmed_flac_mod = {
	&flac_iface, &flac_sig, &flac_destroy
};

//DECODE
static void* flac_in_create(fmed_filt *d);
static void flac_in_free(void *ctx);
static int flac_in_decode(void *ctx, fmed_filt *d);
static const fmed_filter fmed_flac_input = {
	&flac_in_create, &flac_in_decode, &flac_in_free
};

static void flac_meta(flac *f, fmed_filt *d);

//ENCODE
static void* flac_out_create(fmed_filt *d);
static void flac_out_free(void *ctx);
static int flac_out_encode(void *ctx, fmed_filt *d);
static int flac_out_config(ffpars_ctx *conf);
static const fmed_filter fmed_flac_output = {
	&flac_out_create, &flac_out_encode, &flac_out_free, &flac_out_config
};

static int flac_out_addmeta(flac_out *f, fmed_filt *d);

static const ffpars_arg flac_out_conf_args[] = {
	{ "compression",  FFPARS_TINT | FFPARS_F8BIT,  FFPARS_DSTOFF(struct flac_out_conf_t, level) }
	, { "min_meta_size",  FFPARS_TINT,  FFPARS_DSTOFF(struct flac_out_conf_t, min_meta_size) }
};


FF_EXP const fmed_mod* fmed_getmod(const fmed_core *_core)
{
	ffmem_init();
	core = _core;
	return &fmed_flac_mod;
}


static const fmed_filter* flac_iface(const char *name)
{
	if (!ffsz_cmp(name, "decode"))
		return &fmed_flac_input;

	else if (!ffsz_cmp(name, "encode")) {
		flac_out_conf.level = 8;
		flac_out_conf.min_meta_size = 1000;
		return &fmed_flac_output;
	}
	return NULL;
}

static int flac_sig(uint signo)
{
	switch (signo) {
	case FMED_STOP:
		status = 1;
		break;
	}
	return 0;
}

static void flac_destroy(void)
{
}


static void* flac_in_create(fmed_filt *d)
{
	int r;
	int64 total_size;
	flac *f = ffmem_tcalloc1(flac);
	if (f == NULL)
		return NULL;
	ffflac_init(&f->fl);

	if (FFFLAC_RERR == (r = ffflac_open(&f->fl))) {
		errlog(core, d->trk, "flac", "ffflac_open(): %s", ffflac_errstr(&f->fl));
		flac_in_free(f);
		return NULL;
	}

	if (FMED_NULL != (total_size = fmed_getval("total_size")))
		f->fl.total_size = total_size;

	return f;
}

static void flac_in_free(void *ctx)
{
	flac *f = ctx;
	uint i;
	for (i = 0;  i < FFCNT(f->meta);  i++) {
		ffmem_safefree(f->meta[i]);
	}
	ffflac_close(&f->fl);
	ffmem_free(f);
}

static void flac_meta(flac *f, fmed_filt *d)
{
	uint tag;

	dbglog(core, d->trk, "flac", "%S: %S", &f->fl.tagname, &f->fl.tagval);

	if (f->fl.tagval.len == 0)
		return;

	tag = ffflac_tag(f->fl.tagname.ptr, f->fl.tagname.len);
	if (tag >= FFCNT(f->meta))
		return;

	ffmem_safefree(f->meta[tag]);
	if (NULL == (f->meta[tag] = ffsz_alcopy(f->fl.tagval.ptr, f->fl.tagval.len)))
		return;
	d->track->setvalstr(d->trk, metanames[tag], f->meta[tag]);
}

static int flac_in_decode(void *ctx, fmed_filt *d)
{
	enum { I_HDR, I_DATA };
	flac *f = ctx;
	int r;
	int64 seek_time, until_time;

	if (status == 1) {
		d->outlen = 0;
		return FMED_RLASTOUT;
	}

	f->fl.data = (byte*)d->data;
	f->fl.datalen = d->datalen;

again:
	switch (f->state) {
	case I_HDR:
		break;

	case I_DATA:
		if (FMED_NULL != (seek_time = fmed_popval("seek_time")))
			ffflac_seek(&f->fl, ffpcm_samples(seek_time, f->fl.fmt.sample_rate));
		break;
	}

	for (;;) {
		r = ffflac_decode(&f->fl);
		switch (r) {
		case FFFLAC_RMORE:
			if (d->flags & FMED_FLAST) {
				dbglog(core, d->trk, "flac", "file is incomplete");
				d->outlen = 0;
				return FMED_RDONE;
			}
			return FMED_RMORE;

		case FFFLAC_RHDR:
			fmed_setval("pcm_format", f->fl.fmt.format);
			fmed_setval("pcm_channels", f->fl.fmt.channels);
			fmed_setval("pcm_sample_rate", f->fl.fmt.sample_rate);
			fmed_setval("pcm_ileaved", 0);
			fmed_setval("bitrate", ffflac_bitrate(&f->fl));
			fmed_setval("total_samples", ffflac_totalsamples(&f->fl));
			break;

		case FFFLAC_RTAG:
			flac_meta(f, d);
			break;

		case FFFLAC_RHDRFIN:
			f->state = I_DATA;
			goto again;

		case FFFLAC_RDATA:
			goto data;

		case FFFLAC_RSEEK:
			fmed_setval("input_seek", f->fl.off);
			return FMED_RMORE;

		case FFFLAC_RDONE:
			d->outlen = 0;
			return FMED_RDONE;

		case FFFLAC_RERR:
			errlog(core, d->trk, "flac", "ffflac_decode(): %s", ffflac_errstr(&f->fl));
			return FMED_RERR;
		}
	}

data:
	if (FMED_NULL != (until_time = d->track->getval(d->trk, "until_time"))) {
		uint64 until_samples = until_time * f->fl.fmt.sample_rate / 1000;
		if (until_samples <= ffflac_cursample(&f->fl)) {
			dbglog(core, d->trk, "ogg", "until_time is reached");
			d->outlen = 0;
			return FMED_RLASTOUT;
		}
	}

	dbglog(core, d->trk, "flac", "decoded %L samples (%U)"
		, f->fl.pcmlen / ffpcm_size1(&f->fl.fmt), ffflac_cursample(&f->fl));
	fmed_setval("current_position", ffflac_cursample(&f->fl));

	d->data = (void*)f->fl.data;
	d->datalen = f->fl.datalen;
	d->outni = f->fl.pcm;
	d->outlen = f->fl.pcmlen;
	return FMED_ROK;
}


static int flac_out_config(ffpars_ctx *conf)
{
	ffpars_setargs(conf, &flac_out_conf, flac_out_conf_args, FFCNT(flac_out_conf_args));
	return 0;
}

static int flac_out_addmeta(flac_out *f, fmed_filt *d)
{
	uint i;
	const char *val;
	for (i = 0;  i < FFCNT(metanames);  i++) {
		val = d->track->getvalstr(d->trk, metanames[i]);
		if (val != FMED_PNULL) {
			if (0 != ffflac_iaddtag(&f->fl, i, val)) {
				errlog(core, d->trk, "flac", "add meta tag");
				return -1;
			}
		}
	}
	f->fl.min_meta = flac_out_conf.min_meta_size;
	return 0;
}

static void* flac_out_create(fmed_filt *d)
{
	ffpcm fmt;
	flac_out *f = ffmem_tcalloc1(flac_out);
	if (f == NULL)
		return NULL;
	ffflac_enc_init(&f->fl);

	fmed_getpcm(d, &fmt);
	if (1 != fmed_getval("pcm_ileaved")) {
		errlog(core, d->trk, "flac", "input must be interleaved");
		goto fail;
	}

	f->fl.total_samples = fmed_getval("total_samples");
	if ((int64)f->fl.total_samples == FMED_NULL)
		f->fl.total_samples = 0;

	if (0 != flac_out_addmeta(f, d))
		goto fail;

	f->fl.level = flac_out_conf.level;
	if (0 != ffflac_create(&f->fl, &fmt)) {
		errlog(core, d->trk, "flac", "ffflac_create(): %s", ffflac_enc_errstr(&f->fl));
		goto fail;
	}

	return f;

fail:
	flac_out_free(f);
	return NULL;
}

static void flac_out_free(void *ctx)
{
	flac_out *f = ctx;
	ffflac_enc_close(&f->fl);
	ffmem_free(f);
}

static int flac_out_encode(void *ctx, fmed_filt *d)
{
	flac_out *f = ctx;
	int r;

	if (status == 1) {
		errlog(core, d->trk, "flac", "interrupted");
		return FMED_RERR;
	}

	f->fl.pcmi = d->data;
	f->fl.pcmlen = d->datalen;
	r = ffflac_encode(&f->fl);
	d->datalen = f->fl.pcmlen;
	d->data = (void*)f->fl.pcmi;

	switch (r) {
	case FFFLAC_RMORE:
		return FMED_RMORE;

	case FFFLAC_RDATA:
	case FFFLAC_RDONE:
		break;

	case FFFLAC_RERR:
	default:
		errlog(core, d->trk, "flac", "ffflac_encode(): %s", ffflac_enc_errstr(&f->fl));
		return FMED_RERR;
	}

	dbglog(core, d->trk, "flac", "output: %L bytes", f->fl.datalen);
	d->out = (void*)f->fl.data;
	d->outlen = f->fl.datalen;
	if (r == FFFLAC_RDONE)
		return FMED_RDONE;
	return FMED_ROK;
}