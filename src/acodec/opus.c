/** Opus input.
Copyright (c) 2016 Simon Zolin */

#include <fmedia.h>

#include <FF/audio/opus.h>
#include <FF/data/mmtag.h>


static const fmed_core *core;
static const fmed_queue *qu;

//FMEDIA MODULE
static const void* opus_iface(const char *name);
static int opus_sig(uint signo);
static void opus_destroy(void);
static const fmed_mod fmed_opus_mod = {
	&opus_iface, &opus_sig, &opus_destroy
};

//DECODE
static void* opus_open(fmed_filt *d);
static void opus_close(void *ctx);
static int opus_in_decode(void *ctx, fmed_filt *d);
static const fmed_filter opus_input = {
	&opus_open, &opus_in_decode, &opus_close
};


FF_EXP const fmed_mod* fmed_getmod(const fmed_core *_core)
{
	core = _core;
	return &fmed_opus_mod;
}


static const void* opus_iface(const char *name)
{
	if (!ffsz_cmp(name, "decode"))
		return &opus_input;
	return NULL;
}

static int opus_sig(uint signo)
{
	switch (signo) {
	case FMED_SIG_INIT:
		ffmem_init();
		return 0;

	case FMED_OPEN:
		qu = core->getmod("#queue.queue");
		break;
	}
	return 0;
}

static void opus_destroy(void)
{
}


typedef struct opus_in {
	uint state;
	ffopus opus;
	uint64 pagepos;
} opus_in;

static void* opus_open(fmed_filt *d)
{
	opus_in *o;
	if (NULL == (o = ffmem_tcalloc1(opus_in)))
		return NULL;

	if (0 != ffopus_open(&o->opus)) {
		errlog(core, d->trk, NULL, "ffopus_open(): %s", ffopus_errstr(&o->opus));
		ffmem_free(o);
		return NULL;
	}
	return o;
}

static void opus_close(void *ctx)
{
	opus_in *o = ctx;
	ffopus_close(&o->opus);
	ffmem_free(o);
}

static int opus_in_decode(void *ctx, fmed_filt *d)
{
	enum { R_HDR, R_TAGS, R_DATA1, R_DATA };
	opus_in *o = ctx;
	uint64 pos;

	switch (o->state) {
	case R_HDR:
	case R_TAGS:
		if (!(d->flags & FMED_FFWD))
			return FMED_RMORE;

		o->state++;
		break;

	case R_DATA1:
		if ((int64)d->audio.total != FMED_NULL) {
			o->opus.total_samples = d->audio.total;
			d->audio.total -= o->opus.info.preskip;
		}

		if (d->input_info)
			return FMED_RDONE;

		o->state = R_DATA;
		// break

	case R_DATA:
		if ((d->flags & FMED_FFWD) && (int64)d->audio.seek != FMED_NULL) {
			uint64 seek = ffpcm_samples(d->audio.seek, d->audio.fmt.sample_rate);
			ffopus_seek(&o->opus, seek);
			d->audio.seek = FMED_NULL;
		}
		break;
	}

	int r;
	ffstr in = {0};
	if (d->flags & FMED_FFWD) {
		ffstr_set(&in, d->data, d->datalen);
		d->datalen = 0;

		if (o->pagepos != d->audio.pos) {
			o->opus.pos = d->audio.pos;
			o->pagepos = d->audio.pos;
		}
	}

	for (;;) {

	r = ffopus_decode(&o->opus, in.ptr, in.len);

	switch (r) {

	case FFOPUS_RDATA:
		goto data;

	case FFOPUS_RERR:
		errlog(core, d->trk, NULL, "ffopus_decode(): %s", ffopus_errstr(&o->opus));
		return FMED_RERR;

	case FFOPUS_RWARN:
		warnlog(core, d->trk, NULL, "ffopus_decode(): %s", ffopus_errstr(&o->opus));
		// break

	case FFOPUS_RMORE:
		if (d->flags & FMED_FLAST) {
			d->outlen = 0;
			return FMED_RDONE;
		}
		return FMED_RMORE;

	case FFOPUS_RHDR:
		d->track->setvalstr(d->trk, "pcm_decoder", "Opus");
		d->audio.fmt.format = FFPCM_FLOAT;
		d->audio.fmt.channels = o->opus.info.channels;
		d->audio.fmt.sample_rate = o->opus.info.rate;
		d->audio.fmt.ileaved = 1;
		return FMED_RMORE;

	case FFOPUS_RTAG: {
		const ffvorbtag *vtag = &o->opus.vtag;
		dbglog(core, d->trk, NULL, "%S: %S", &vtag->name, &vtag->val);
		ffstr name = vtag->name;
		if (vtag->tag != 0)
			ffstr_setz(&name, ffmmtag_str[vtag->tag]);
		qu->meta_set((void*)fmed_getval("queue_item"), name.ptr, name.len, vtag->val.ptr, vtag->val.len, FMED_QUE_TMETA);
		break;
	}

	case FFOPUS_RHDRFIN:
		return FMED_RMORE;
	}
	}

data:
	pos = ffopus_pos(&o->opus);
	dbglog(core, d->trk, NULL, "decoded %u samples (%U)"
		, o->opus.pcm.len / ffpcm_size1(&d->audio.fmt), pos);
	d->audio.pos = pos - o->opus.pcm.len / ffpcm_size1(&d->audio.fmt);
	d->out = o->opus.pcm.ptr,  d->outlen = o->opus.pcm.len;
	return FMED_RDATA;
}