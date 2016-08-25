/** MP4 (AAC, ALAC) input.
Copyright (c) 2016 Simon Zolin */

#include <fmedia.h>

#include <FF/data/mp4.h>
#include <FF/data/mmtag.h>
#include <FF/audio/alac.h>
#include <FF/audio/aac.h>


static const fmed_core *core;
static const fmed_queue *qu;

static struct aac_out_conf_t {
	uint aot;
	uint qual;
	uint afterburner;
	uint bandwidth;
} aac_out_conf;


struct mp4;
struct dec_iface_t {
	int (*open)(struct mp4 *m, fmed_filt *d);
	void (*close)(struct mp4 *m);
	int (*decode)(struct mp4 *m, fmed_filt *d);
};

typedef struct mp4 {
	ffmp4 mp;
	uint state;

	union {
	ffalac alac;
	ffaac aac;
	};
	uint64 seek;

	const struct dec_iface_t *dec;
} mp4;

typedef struct mp4_out {
	uint state;
	ffpcm fmt;
	ffmp4_cook mp;
	ffaac_enc aac;
} mp4_out;


//FMEDIA MODULE
static const void* mp4_iface(const char *name);
static int mp4_sig(uint signo);
static void mp4_destroy(void);
static const fmed_mod fmed_mp4_mod = {
	&mp4_iface, &mp4_sig, &mp4_destroy
};

//DECODE
static void* mp4_in_create(fmed_filt *d);
static void mp4_in_free(void *ctx);
static int mp4_in_decode(void *ctx, fmed_filt *d);
static const fmed_filter fmed_mp4_input = {
	&mp4_in_create, &mp4_in_decode, &mp4_in_free
};

//AAC ENCODE
static int aac_out_config(ffpars_ctx *ctx);
static void* mp4_out_create(fmed_filt *d);
static void mp4_out_free(void *ctx);
static int mp4_out_encode(void *ctx, fmed_filt *d);
static const fmed_filter mp4aac_output = {
	&mp4_out_create, &mp4_out_encode, &mp4_out_free, &aac_out_config
};

static void mp4_meta(mp4 *m, fmed_filt *d);


static int mp4aac_open(mp4 *m, fmed_filt *d);
static void mp4aac_close(mp4 *m);
static int mp4aac_decode(mp4 *m, fmed_filt *d);
static const struct dec_iface_t aac_dec_iface = {
	&mp4aac_open, &mp4aac_close, &mp4aac_decode
};

static int mp4alac_open(mp4 *m, fmed_filt *d);
static void mp4alac_close(mp4 *m);
static int mp4alac_decode(mp4 *m, fmed_filt *d);
static const struct dec_iface_t alac_dec_iface = {
	&mp4alac_open, &mp4alac_close, &mp4alac_decode
};

static const ffpars_arg aac_out_conf_args[] = {
	{ "profile",	FFPARS_TINT,  FFPARS_DSTOFF(struct aac_out_conf_t, aot) },
	{ "quality",	FFPARS_TINT,  FFPARS_DSTOFF(struct aac_out_conf_t, qual) },
	{ "afterburner",	FFPARS_TINT,  FFPARS_DSTOFF(struct aac_out_conf_t, afterburner) },
	{ "bandwidth",	FFPARS_TINT,  FFPARS_DSTOFF(struct aac_out_conf_t, bandwidth) },
};


FF_EXP const fmed_mod* fmed_getmod(const fmed_core *_core)
{
	ffmem_init();
	core = _core;
	return &fmed_mp4_mod;
}


static const void* mp4_iface(const char *name)
{
	if (!ffsz_cmp(name, "decode"))
		return &fmed_mp4_input;
	else if (!ffsz_cmp(name, "aac-encode"))
		return &mp4aac_output;
	return NULL;
}

static int mp4_sig(uint signo)
{
	switch (signo) {
	case FMED_OPEN:
		qu = core->getmod("#queue.queue");
		break;
	}
	return 0;
}

static void mp4_destroy(void)
{
}


static void* mp4_in_create(fmed_filt *d)
{
	int64 val;
	mp4 *m = ffmem_tcalloc1(mp4);
	if (m == NULL)
		return NULL;

	ffmp4_init(&m->mp);
	m->seek = (uint64)-1;

	if (FMED_NULL != (val = fmed_getval("total_size")))
		m->mp.total_size = val;

	return m;
}

static void mp4_in_free(void *ctx)
{
	mp4 *m = ctx;
	if (m->dec != NULL)
		m->dec->close(m);
	ffmp4_close(&m->mp);
	ffmem_free(m);
}

static void mp4_meta(mp4 *m, fmed_filt *d)
{
	ffstr name, val;
	if (m->mp.tag == 0)
		return;
	ffstr_setz(&name, ffmmtag_str[m->mp.tag]);
	val = m->mp.tagval;

	dbglog(core, d->trk, "mp4", "tag: %S: %S", &name, &val);

	qu->meta_set((void*)fmed_getval("queue_item"), name.ptr, name.len, val.ptr, val.len, FMED_QUE_TMETA);
}

static int mp4_in_decode(void *ctx, fmed_filt *d)
{
	enum { I_HDR, I_DATA, I_DECODE, };
	mp4 *m = ctx;
	int r;
	int64 val;

	if (d->flags & FMED_FSTOP) {
		d->outlen = 0;
		return FMED_RLASTOUT;
	}

	m->mp.data = d->data;
	m->mp.datalen = d->datalen;

	for (;;) {

	switch (m->state) {

	case I_DATA:
		if (FMED_NULL != (val = fmed_popval("seek_time"))) {
			m->seek = ffpcm_samples(val, m->mp.fmt.sample_rate);
			ffmp4_seek(&m->mp, m->seek);
		}

	case I_HDR:
		r = ffmp4_read(&m->mp);
		switch (r) {
		case FFMP4_RMORE:
			if (d->flags & FMED_FLAST) {
				warnlog(core, d->trk, "mp4", "file is incomplete");
				d->outlen = 0;
				return FMED_RDONE;
			}
			return FMED_RMORE;

		case FFMP4_RHDR:
			d->track->setvalstr(d->trk, "pcm_decoder", ffmp4_codec(m->mp.codec));
			fmed_setval("pcm_format", m->mp.fmt.format);
			fmed_setval("pcm_channels", m->mp.fmt.channels);
			fmed_setval("pcm_sample_rate", m->mp.fmt.sample_rate);

			fmed_setval("total_samples", ffmp4_totalsamples(&m->mp));

			if (m->mp.codec == FFMP4_ALAC)
				m->dec = &alac_dec_iface;
			else if (m->mp.codec == FFMP4_AAC)
				m->dec = &aac_dec_iface;
			else {
				errlog(core, d->trk, "mp4", "%s: decoding unsupported", ffmp4_codec(m->mp.codec));
				return FMED_RERR;
			}

			if (0 != (r = m->dec->open(m, d)))
				return r;

			if (FMED_NULL != fmed_getval("input_info"))
				break;

			fmed_setval("pcm_ileaved", 1);
			break;

		case FFMP4_RTAG:
			mp4_meta(m, d);
			break;

		case FFMP4_RMETAFIN:
			if (FMED_NULL != fmed_getval("input_info"))
				return FMED_ROK;

			m->state = I_DATA;
			continue;

		case FFMP4_RDATA:
			m->state = I_DECODE;
			continue;

		case FFMP4_RDONE:
			d->outlen = 0;
			return FMED_RLASTOUT;

		case FFMP4_RSEEK:
			fmed_setval("input_seek", m->mp.off);
			return FMED_RMORE;

		case FFMP4_RWARN:
			warnlog(core, d->trk, "mp4", "ffmp4_read(): at offset 0x%xU: %s"
				, m->mp.off, ffmp4_errstr(&m->mp));
			break;

		case FFMP4_RERR:
			errlog(core, d->trk, "mp4", "ffmp4_read(): %s", ffmp4_errstr(&m->mp));
			return FMED_RERR;
		}
		break;

	case I_DECODE:
		r = m->dec->decode(m, d);
		if (r == FMED_RMORE) {
			m->state = I_DATA;
			continue;
		}
		return r;
	}

	}

	//unreachable
}


static int mp4aac_open(mp4 *m, fmed_filt *d)
{
	if (0 != ffaac_open(&m->aac, m->mp.fmt.channels, m->mp.out, m->mp.outlen)) {
		errlog(core, d->trk, "mp4", "ffaac_open(): %s", ffaac_errstr(&m->aac));
		return FMED_RERR;
	}

	uint br = (m->mp.aac_brate != 0) ? m->mp.aac_brate : ffmp4_bitrate(&m->mp);
	fmed_setval("bitrate", br);
	return 0;
}

static void mp4aac_close(mp4 *m)
{
	ffaac_close(&m->aac);
}

static int mp4aac_decode(mp4 *m, fmed_filt *d)
{
	if (m->mp.outlen != 0) {
		m->aac.data = m->mp.out;
		m->aac.datalen = m->mp.outlen;
		if (m->seek != (uint64)-1) {
			ffalac_seek(&m->alac, m->seek);
			m->seek = (uint64)-1;
		}
		m->mp.outlen = 0;
	}

	int r;
	r = ffaac_decode(&m->aac);
	if (r == FFAAC_RERR) {
		errlog(core, d->trk, "mp4", "ffaac_decode(): %s", ffaac_errstr(&m->aac));
		return FMED_RERR;

	} else if (r == FFAAC_RMORE)
		return FMED_RMORE;

	dbglog(core, d->trk, "mp4", "AAC: decoded %u samples (%U)"
		, m->aac.pcmlen / ffpcm_size1(&m->aac.fmt), ffmp4_cursample(&m->mp));
	fmed_setval("current_position", ffmp4_cursample(&m->mp));

	d->data = (void*)m->mp.data;
	d->datalen = m->mp.datalen;
	d->out = (void*)m->aac.pcm;
	d->outlen = m->aac.pcmlen;
	return FMED_RDATA;
}


static int mp4alac_open(mp4 *m, fmed_filt *d)
{
	if (0 != ffalac_open(&m->alac, m->mp.out, m->mp.outlen)) {
		errlog(core, d->trk, "mp4", "ffalac_open(): %s", ffalac_errstr(&m->alac));
		return FMED_RERR;
	}
	if (0 != memcmp(&m->alac.fmt, &m->mp.fmt, sizeof(m->alac.fmt))) {
		errlog(core, d->trk, "mp4", "ALAC: audio format doesn't match with format from MP4");
		return FMED_RERR;
	}

	uint br = (m->alac.bitrate != 0) ? m->alac.bitrate : ffmp4_bitrate(&m->mp);
	fmed_setval("bitrate", br);
	return 0;
}

static void mp4alac_close(mp4 *m)
{
	ffalac_close(&m->alac);
}

static int mp4alac_decode(mp4 *m, fmed_filt *d)
{
	if (m->mp.outlen != 0) {
		m->alac.data = m->mp.out;
		m->alac.datalen = m->mp.outlen;
		m->alac.cursample = ffmp4_cursample(&m->mp);
		if (m->seek != (uint64)-1) {
			ffalac_seek(&m->alac, m->seek);
			m->seek = (uint64)-1;
		}
		m->mp.outlen = 0;
	}

	int r;
	r = ffalac_decode(&m->alac);
	if (r == FFALAC_RERR) {
		errlog(core, d->trk, "mp4", "ffalac_decode(): %s", ffalac_errstr(&m->alac));
		return FMED_RERR;

	} else if (r == FFALAC_RMORE)
		return FMED_RMORE;

	dbglog(core, d->trk, "mp4", "ALAC: decoded %u samples (%U)"
		, m->alac.pcmlen / ffpcm_size1(&m->alac.fmt), ffalac_cursample(&m->alac));
	fmed_setval("current_position", ffalac_cursample(&m->alac));

	d->data = (void*)m->mp.data;
	d->datalen = m->mp.datalen;
	d->out = m->alac.pcm;
	d->outlen = m->alac.pcmlen;
	return FMED_RDATA;
}


static int aac_out_config(ffpars_ctx *ctx)
{
	aac_out_conf.aot = AAC_LC;
	aac_out_conf.qual = 256;
	aac_out_conf.afterburner = 1;
	aac_out_conf.bandwidth = 0;
	ffpars_setargs(ctx, &aac_out_conf, aac_out_conf_args, FFCNT(aac_out_conf_args));
	return 0;
}

static void* mp4_out_create(fmed_filt *d)
{
	mp4_out *m = ffmem_tcalloc1(mp4_out);
	if (m == NULL)
		return NULL;
	return m;
}

static void mp4_out_free(void *ctx)
{
	mp4_out *m = ctx;
	ffaac_enc_close(&m->aac);
	ffmp4_wclose(&m->mp);
	ffmem_free(m);
}

static int mp4_out_encode(void *ctx, fmed_filt *d)
{
	mp4_out *m = ctx;
	int r;

	enum { I_CONV, I_INIT, I_ENC, I_MP4 };

	for (;;) {
	switch (m->state) {

	case I_CONV:
		fmed_setval("conv_pcm_format", FFPCM_16);
		fmed_setval("conv_pcm_ileaved", 1);
		m->state = I_INIT;
		return FMED_RMORE;

	case I_INIT: {
		fmed_getpcm(d, &m->fmt);

		if (m->fmt.format != FFPCM_16LE || 1 != fmed_getval("pcm_ileaved")) {
			errlog(core, d->trk, NULL, "unsupported input PCM format");
			return FMED_RERR;
		}

		int64 total_samples;
		if (FMED_NULL == (total_samples = fmed_getval("total_samples"))) {
			errlog(core, d->trk, NULL, "total_samples unknown");
			return FMED_RERR;
		}

		int qual;
		if (FMED_NULL == (qual = fmed_getval("aac-quality")))
			qual = aac_out_conf.qual;
		if (qual > 5 && qual < 8000)
			qual *= 1000;

		m->aac.info.aot = aac_out_conf.aot;
		m->aac.info.afterburner = aac_out_conf.afterburner;
		m->aac.info.bandwidth = aac_out_conf.bandwidth;

		if (0 != (r = ffaac_create(&m->aac, &m->fmt, qual))) {
			errlog(core, d->trk, NULL, "ffaac_create(): %s", ffaac_enc_errstr(&m->aac));
			return FMED_RERR;
		}

		uint delay_frames = m->aac.info.enc_delay / m->aac.info.frame_samples + !!(m->aac.info.enc_delay % m->aac.info.frame_samples);
		total_samples += delay_frames * m->aac.info.frame_samples;
		ffstr asc = ffaac_enc_conf(&m->aac);
		if (0 != (r = ffmp4_create_aac(&m->mp, &m->fmt, &asc, total_samples, ffaac_enc_frame_samples(&m->aac)))) {
			errlog(core, d->trk, NULL, "ffmp4_create_aac(): %s", ffmp4_werrstr(&m->mp));
			return FMED_RERR;
		}
		m->state = I_ENC;
		// break
	}

	case I_ENC:
		if (d->flags & FMED_FLAST)
			m->aac.fin = 1;
		m->aac.pcm = (void*)d->data,  m->aac.pcmlen = d->datalen;
		r = ffaac_encode(&m->aac);
		switch (r) {
		case FFAAC_RDONE:
			m->mp.fin = 1;
			m->state = I_MP4;
			continue;

		case FFAAC_RMORE:
			return FMED_RMORE;

		case FFAAC_RDATA:
			break;

		case FFAAC_RERR:
			errlog(core, d->trk, NULL, "ffaac_encode(): %s", ffaac_enc_errstr(&m->aac));
			return FMED_RERR;
		}
		dbglog(core, d->trk, NULL, "encoded %L samples into %L bytes"
			, (d->datalen - m->aac.pcmlen) / ffpcm_size1(&m->fmt), m->aac.datalen);
		d->data = (void*)m->aac.pcm,  d->datalen = m->aac.pcmlen;
		m->mp.data = m->aac.data,  m->mp.datalen = m->aac.datalen;
		m->state = I_MP4;
		// break

	case I_MP4:
		r = ffmp4_write(&m->mp);
		switch (r) {
		case FFMP4_RMORE:
			m->state = I_ENC;
			continue;

		case FFMP4_RSEEK:
			fmed_setval("output_seek", m->mp.off);
			continue;

		case FFMP4_RDATA:
			d->out = m->mp.out,  d->outlen = m->mp.outlen;
			return FMED_RDATA;

		case FFMP4_RDONE:
			d->outlen = 0;
			return FMED_RDONE;

		case FFMP4_RWARN:
			warnlog(core, d->trk, NULL, "ffmp4_write(): %s", ffmp4_werrstr(&m->mp));
			continue;

		case FFMP4_RERR:
			errlog(core, d->trk, NULL, "ffmp4_write(): %s", ffmp4_werrstr(&m->mp));
			return FMED_RERR;
		}
	}
	}
}
