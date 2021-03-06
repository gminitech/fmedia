/** Terminal UI.
Copyright (c) 2015 Simon Zolin */

#include <fmedia.h>

#include <FF/audio/pcm.h>
#include <FF/array.h>
#include <FFOS/error.h>


struct tui;

typedef struct gtui {
	const fmed_queue *qu;
	const fmed_track *track;
	ffkevent kev;

	fflock lktrk;
	struct tui *curtrk; //currently playing track
	struct tui *curtrk_rec; //currently recording track

	uint vol;
	uint progress_dots;

	uint mute :1;
} gtui;

static gtui *gt;

typedef struct tui {
	fmed_filt *d;
	void *trk;
	fmed_que_entry *qent;
	uint64 total_samples;
	uint64 played_samples;
	uint lastpos;
	uint sample_rate;
	uint sampsize;
	uint total_time_sec;
	ffstr3 buf;
	double maxdb;
	uint nback;

	uint goback :1
		, rec :1
		, conversion :1
		, paused :1
		;
} tui;

static struct tui_conf_t {
	byte echo_off;
} tui_conf;

enum {
	SEEK_STEP = 5 * 1000,
	SEEK_STEP_MED = 15 * 1000,
	SEEK_STEP_LARGE = 60 * 1000,
	REC_STATUS_UPDATE = 200, //update recording status timeout (msec)

	VOL_STEP = 5,
	VOL_MAX = 125,
	VOL_LO = /*-*/48,
	VOL_HI = 6,

	MINDB = 40,
};

enum CMDS {
	CMD_PLAY,
	CMD_STOP,
	CMD_NEXT,
	CMD_PREV,
	CMD_SEEKRIGHT,
	CMD_SEEKLEFT,
	CMD_VOLUP,
	CMD_VOLDOWN,
	CMD_MUTE,

	CMD_RM,
	CMD_DELFILE,
	CMD_SHOWTAGS,
	CMD_SAVETRK,

	CMD_QUIT,

	CMD_MASK = 0xff,

	_CMD_CURTRK_REC = 1 << 26,
	_CMD_F3 = 1 << 27, //use 'cmdfunc3'
	_CMD_CURTRK = 1 << 28,
	_CMD_CORE = 1 << 29,
	_CMD_F1 = 1 << 31, //use 'cmdfunc1'
};

typedef void (*cmdfunc)(tui *t, uint cmd);
typedef void (*cmdfunc3)(tui *t, uint cmd, void *udata);
typedef void (*cmdfunc1)(uint cmd);

static const fmed_core *core;

//FMEDIA MODULE
static const void* tui_iface(const char *name);
static int tui_mod_conf(const char *name, ffpars_ctx *conf);
static int tui_sig(uint signo);
static void tui_destroy(void);
static const fmed_mod fmed_tui_mod = {
	.ver = FMED_VER_FULL, .ver_core = FMED_VER_CORE,
	&tui_iface, &tui_sig, &tui_destroy, &tui_mod_conf
};

static void* tui_open(fmed_filt *d);
static int tui_process(void *ctx, fmed_filt *d);
static void tui_close(void *ctx);
static int tui_config(ffpars_ctx *conf);
static const fmed_filter fmed_tui = {
	&tui_open, &tui_process, &tui_close
};

static void tui_info(tui *t, fmed_filt *d);
static void tui_cmdread(void *param);
static void tui_help(uint cmd);
static void tui_rmfile(tui *t, uint cmd);
static int tui_setvol(tui *t, uint vol);
static void tui_vol(tui *t, uint cmd);
static void tui_seek(tui *t, uint cmd, void *udata);
static void tui_op_trk(struct tui *t, uint cmd);

struct key;
static void tui_corecmd(void *param);
static void tui_corecmd_add(const struct key *k, void *udata);


static const ffpars_arg tui_conf_args[] = {
	{ "echo_off",	FFPARS_TBOOL | FFPARS_F8BIT,  FFPARS_DSTOFF(struct tui_conf_t, echo_off) },
};


FF_EXP const fmed_mod* fmed_getmod(const fmed_core *_core)
{
	core = _core;
	return &fmed_tui_mod;
}


static const void* tui_iface(const char *name)
{
	if (!ffsz_cmp(name, "tui")) {
		return &fmed_tui;
	}
	return NULL;
}

static int tui_mod_conf(const char *name, ffpars_ctx *ctx)
{
	if (!ffsz_cmp(name, "tui"))
		return tui_config(ctx);
	return -1;
}

static int tui_sig(uint signo)
{
	switch (signo) {
	case FMED_SIG_INIT:
		ffmem_init();
		break;

	case FMED_OPEN:
		gt = ffmem_tcalloc1(gtui);
		fflk_init(&gt->lktrk);
		gt->vol = 100;
		uint term_wnd_size = 80;
		if (NULL == (gt->qu = core->getmod("#queue.queue")))
			return 1;
		if (NULL == (gt->track = core->getmod("#core.track")))
			return 1;

		if (core->props->stdin_busy)
			return 0;

		uint attr = FFSTD_LINEINPUT;
		if (tui_conf.echo_off)
			attr |= FFSTD_ECHO;
		ffstd_attr(ffstdin, attr, 0);

#ifdef FF_WIN
		CONSOLE_SCREEN_BUFFER_INFO info = {};
		if (GetConsoleScreenBufferInfo(ffstdout, &info))
			term_wnd_size = info.dwSize.X;

		if (0 != core->cmd(FMED_WOH_INIT)) {
			fmed_warnlog(core, NULL, "tui", "can't start stdin reader");
			return 0;
		}
		fftask t;
		t.handler = &tui_cmdread;
		t.param = gt;
		if (0 != core->cmd(FMED_WOH_ADD, ffstdin, &t)) {
			fmed_warnlog(core, NULL, "tui", "can't start stdin reader");
			return 0;
		}
#else
		fffile_nblock(ffstdin, 1);
		ffkev_init(&gt->kev);
		gt->kev.oneshot = 0;
		gt->kev.fd = ffstdin;
		gt->kev.handler = tui_cmdread;
		gt->kev.udata = gt;
		if (0 != ffkev_attach(&gt->kev, core->kq, FFKQU_READ)) {
			fmed_syswarnlog(core, NULL, "tui", "ffkev_attach()");
			return 0;
		}
#endif

		gt->progress_dots = ffmax((int)term_wnd_size - (int)FFSLEN("[] 00:00 / 00:00"), 0);
		break;
	}
	return 0;
}

static void tui_destroy(void)
{
	if (gt == NULL)
		return;

#ifdef FF_WIN
	core->cmd(FMED_WOH_DEL, ffstdin);
#endif

	uint attr = FFSTD_LINEINPUT;
	if (tui_conf.echo_off)
		attr |= FFSTD_ECHO;
	ffstd_attr(ffstdin, attr, attr);

	ffmem_safefree(gt);
}


static int tui_config(ffpars_ctx *conf)
{
	tui_conf.echo_off = 1;
	ffpars_setargs(conf, &tui_conf, tui_conf_args, FFCNT(tui_conf_args));
	return 0;
}

static void* tui_open(fmed_filt *d)
{
	tui *t = ffmem_tcalloc1(tui);
	if (t == NULL)
		return NULL;
	t->lastpos = (uint)-1;
	t->d = d;

	t->qent = (void*)fmed_getval("queue_item");

	if (d->type == FMED_TRK_TYPE_REC) {
		t->rec = 1;
		gt->curtrk_rec = t;
		t->maxdb = -MINDB;

		ffpcm fmt;
		ffpcm_fmtcopy(&fmt, &d->audio.fmt);
		t->sample_rate = fmt.sample_rate;
		t->sampsize = ffpcm_size1(&fmt);

		core->log(FMED_LOG_USER, d->trk, NULL, "Recording...  Source: %s %uHz %s.  %sPress \"s\" to stop."
			, ffpcm_fmtstr(fmt.format), fmt.sample_rate, ffpcm_channelstr(fmt.channels)
			, (d->a_prebuffer != 0) ? "Press \"T\" to start writing to a file.  " : "");
	}

	t->trk = d->trk;

	if (FMED_PNULL != d->track->getvalstr(d->trk, "output"))
		t->conversion = 1;
	else if (t->qent != FMED_PNULL) {
		fflk_lock(&gt->lktrk);
		gt->curtrk = t;
		fflk_unlock(&gt->lktrk);
	}

	uint vol = (gt->mute) ? 0 : gt->vol;
	if (vol != 100 && !t->conversion)
		tui_setvol(t, vol);

	d->meta_changed = 1;
	return t;
}

static void tui_close(void *ctx)
{
	tui *t = ctx;

	if (t == gt->curtrk) {
		fflk_lock(&gt->lktrk);
		gt->curtrk = NULL;
		fflk_unlock(&gt->lktrk);
	}
	if (t == gt->curtrk_rec)
		gt->curtrk_rec = NULL;
	ffarr_free(&t->buf);
	ffmem_free(t);
}

static void tui_addtags(tui *t, fmed_que_entry *qent, ffarr *buf)
{
	fmed_trk_meta meta;
	ffmem_tzero(&meta);
	while (0 == t->d->track->cmd2(t->d->trk, FMED_TRACK_META_ENUM, &meta)) {
		ffstr_catfmt(buf, "%S\t%S\n", &meta.name, &meta.val);
	}
}

static void tui_info(tui *t, fmed_filt *d)
{
	uint64 total_time, tsize;
	uint tmsec;
	const char *input;
	ffstr *tstr, artist = {0}, title = {0};
	ffpcm fmt;

	ffpcm_fmtcopy(&fmt, &d->audio.fmt);
	t->sample_rate = fmt.sample_rate;
	t->sampsize = ffpcm_size1(&fmt);

	if (FMED_PNULL == (input = d->track->getvalstr(d->trk, "input")))
		return;

	total_time = ((int64)t->total_samples != FMED_NULL) ? ffpcm_time(t->total_samples, t->sample_rate) : 0;
	tmsec = (uint)(total_time / 1000);
	t->total_time_sec = tmsec;

	tsize = ((int64)d->input.size != FMED_NULL) ? d->input.size : 0;

	if (FMED_PNULL != (tstr = (void*)d->track->getvalstr3(d->trk, "artist", FMED_TRK_META | FMED_TRK_VALSTR)))
		artist = *tstr;

	if (FMED_PNULL != (tstr = (void*)d->track->getvalstr3(d->trk, "title", FMED_TRK_META | FMED_TRK_VALSTR)))
		title = *tstr;

	fmed_que_entry *qtrk = (void*)d->track->getval(d->trk, "queue_item");
	size_t trkid = (qtrk != FMED_PNULL) ? gt->qu->cmdv(FMED_QUE_ID, qtrk) + 1 : 1;

	t->buf.len = 0;
	ffstr_catfmt(&t->buf, "\n#%L \"%S - %S\" %s %.02F MB, %u:%02u.%03u (%,U samples), %u kbps, %s, %u Hz, %s, %s"
		, trkid
		, &artist, &title
		, input
		, (double)tsize / (1024 * 1024)
		, tmsec / 60, tmsec % 60, (uint)(total_time % 1000)
		, t->total_samples
		, (d->audio.bitrate + 500) / 1000
		, d->audio.decoder
		, fmt.sample_rate
		, ffpcm_fmtstr(fmt.format)
		, ffpcm_channelstr(fmt.channels));

	if (d->video.width != 0) {
		ffstr_catfmt(&t->buf, "  Video: %s, %ux%u"
			, d->video.decoder, (int)d->video.width, (int)d->video.height);
	}

	ffstr_catfmt(&t->buf, "\n\n");

	if (d->show_tags) {
		tui_addtags(t, t->qent, &t->buf);
	}

	ffstd_write(ffstderr, t->buf.ptr, t->buf.len);
	t->buf.len = 0;
}

static void tui_seek(tui *t, uint cmd, void *udata)
{
	int64 pos = (uint64)t->lastpos * 1000;
	uint by;
	switch ((size_t)udata & FFKEY_MODMASK) {
	case 0:
		by = SEEK_STEP;
		break;
	case FFKEY_ALT:
		by = SEEK_STEP_MED;
		break;
	case FFKEY_CTRL:
		by = SEEK_STEP_LARGE;
		break;
	default:
		return;
	}
	if (cmd == CMD_SEEKRIGHT)
		pos += by;
	else
		pos = ffmax(pos - by, 0);
	t->d->audio.seek = pos;
	t->d->snd_output_clear = 1;
	t->goback = 1;
}

static void tui_vol(tui *t, uint cmd)
{
	uint vol = 0;

	switch (cmd & ~FFKEY_MODMASK) {
	case CMD_VOLUP:
		vol = gt->vol = ffmin(gt->vol + VOL_STEP, VOL_MAX);
		gt->mute = 0;
		break;

	case CMD_VOLDOWN:
		vol = gt->vol = ffmax((int)gt->vol - VOL_STEP, 0);
		gt->mute = 0;
		break;

	case CMD_MUTE:
		gt->mute = !gt->mute;
		vol = (gt->mute) ? 0 : gt->vol;
		break;
	}

	int db = tui_setvol(t, vol);
	core->log(FMED_LOG_USER, t->d->trk, NULL, "Volume: %.02FdB", (double)db / 100);
}

static int tui_setvol(tui *t, uint vol)
{
	int db;
	if (vol <= 100)
		db = ffpcm_vol2db(vol, VOL_LO) * 100;
	else
		db = ffpcm_vol2db_inc(vol - 100, VOL_MAX - 100, VOL_HI) * 100;
	t->d->audio.gain = db;
	return db;
}

static void tui_rmfile(tui *t, uint cmd)
{
	fmed_que_entry *qtrk = t->qent;
	if ((cmd & CMD_MASK) == CMD_DELFILE) {
		ffarr fn = {0};
		if (0 != ffstr_catfmt(&fn, "%S.deleted%Z", &qtrk->url)
			&& 0 == fffile_rename(qtrk->url.ptr, fn.ptr))
			gt->qu->cmd(FMED_QUE_RM, qtrk);
		else
			syserrlog(core, t->trk, "tui", "%s", "can't rename file");
		ffarr_free(&fn);
	} else
		gt->qu->cmd(FMED_QUE_RM, qtrk);
}

static int tui_process(void *ctx, fmed_filt *d)
{
	tui *t = ctx;
	int64 playpos;
	uint playtime;

	if (d->meta_block) {
		goto pass;
	}

	if (t->rec) {
		double db = d->audio.maxpeak;
		if (t->maxdb < db)
			t->maxdb = db;

		playtime = ffpcm_time(d->audio.pos, t->sample_rate);
		if (playtime / REC_STATUS_UPDATE == t->lastpos / REC_STATUS_UPDATE)
			goto done;
		t->lastpos = playtime;
		playtime /= 1000;

		if (db < -MINDB)
			db = -MINDB;

		size_t pos = ((MINDB + db) / MINDB) * 10;
		t->buf.len = 0;
		ffstr_catfmt(&t->buf, "%*c%u:%02u  [%*c%*c] %3.02FdB / %.02FdB  "
			, (size_t)t->nback, '\r'
			, playtime / 60, playtime % 60
			, pos, '='
			, (size_t)(10 - pos), '.'
			, db, t->maxdb);

		goto print;
	}

	if (t->goback) {
		t->goback = 0;
		return FMED_RMORE;
	}

	if (d->meta_changed) {
		d->meta_changed = 0;

		if (d->audio.fmt.format == 0) {
			errlog(core, d->trk, NULL, "audio format isn't set");
			return FMED_RERR;
		}

		t->total_samples = d->audio.total;
		t->played_samples = 0;
		tui_info(t, d);
		if (d->input_info)
			return FMED_RFIN;
	}

	if (core->props->parallel) {
		/* We don't print progress bars because it would be a mess. */
		d->out = d->data;
		d->outlen = d->datalen;
		return FMED_RDONE;
	}

	if (gt->curtrk_rec != NULL && !t->rec)
		goto done; //don't show playback bar while recording in another track

	if (FMED_NULL == (playpos = d->audio.pos))
		playpos = t->played_samples;
	playtime = (uint)(ffpcm_time(playpos, t->sample_rate) / 1000);
	if (playtime == t->lastpos) {
		goto done;
	}
	t->lastpos = playtime;

	if ((int64)t->total_samples == FMED_NULL
		|| (uint64)playpos >= t->total_samples) {

		t->buf.len = 0;
		ffstr_catfmt(&t->buf, "%*c%u:%02u"
			, (size_t)t->nback, '\r'
			, playtime / 60, playtime % 60);

		goto print;
	}

	t->buf.len = 0;
	uint dots = gt->progress_dots;
	ffstr_catfmt(&t->buf, "%*c[%*c%*c] %u:%02u / %u:%02u"
		, (size_t)t->nback, '\r'
		, (size_t)(playpos * dots / t->total_samples), '='
		, (size_t)(dots - (playpos * dots / t->total_samples)), '.'
		, playtime / 60, playtime % 60
		, t->total_time_sec / 60, t->total_time_sec % 60);

print:
	fffile_write(ffstderr, t->buf.ptr, t->buf.len);
	t->nback = 1;
	if (core->loglev == FMED_LOG_DEBUG)
		t->nback = 0;
	t->buf.len = 0;

done:
	t->played_samples += d->datalen / t->sampsize;
	dbglog(core, d->trk, NULL, "samples: +%L [%U] at %U"
		, d->datalen / t->sampsize, t->played_samples, d->audio.pos);

pass:
	d->out = d->data;
	d->outlen = d->datalen;
	d->datalen = 0;

	if (d->flags & FMED_FLAST) {
		fffile_write(ffstderr, "\n", 1);
		return FMED_RDONE;
	}
	return FMED_ROK;
}

static void tui_op_trk(struct tui *t, uint cmd)
{
	switch (cmd) {
	case CMD_SHOWTAGS:
		t->buf.len = 0;
		tui_addtags(t, t->qent, &t->buf);
		ffstd_write(ffstderr, t->buf.ptr, t->buf.len);
		t->buf.len = 0;
		break;

	case CMD_SAVETRK:
		fmed_infolog(core, t->trk, "tui", "Saving track to disk");
		t->d->save_trk = 1;
		break;
	}
}

static void tui_op(uint cmd)
{
	switch (cmd) {
	case CMD_STOP:
		gt->track->cmd((void*)-1, FMED_TRACK_STOPALL);
		break;

	case CMD_PLAY:
		if (gt->curtrk == NULL) {
			gt->qu->cmd(FMED_QUE_PLAY, NULL);
			break;
		}

		if (gt->curtrk->paused) {
			gt->curtrk->paused = 0;
			gt->curtrk->d->snd_output_pause = 0;
			gt->track->cmd(gt->curtrk->trk, FMED_TRACK_UNPAUSE);
			break;
		}

		gt->curtrk->d->snd_output_pause = 1;
		gt->curtrk->paused = 1;
		break;

	case CMD_NEXT:
		if (gt->curtrk != NULL)
			gt->track->cmd(gt->curtrk->trk, FMED_TRACK_STOP);
		gt->qu->cmd(FMED_QUE_NEXT2, (gt->curtrk != NULL) ? gt->curtrk->qent : NULL);
		break;

	case CMD_PREV:
		if (gt->curtrk != NULL)
			gt->track->cmd(gt->curtrk->trk, FMED_TRACK_STOP);
		gt->qu->cmd(FMED_QUE_PREV2, (gt->curtrk != NULL) ? gt->curtrk->qent : NULL);
		break;

	case CMD_QUIT:
		gt->track->cmd(NULL, FMED_TRACK_STOPALL_EXIT);
		break;
	}
}


struct key {
	uint key;
	uint cmd;
	void *func; // cmdfunc | cmdfunc1
};

static struct key hotkeys[] = {
	{ ' ',	CMD_PLAY | _CMD_F1 | _CMD_CORE,	&tui_op },
	{ 'D',	CMD_DELFILE | _CMD_CURTRK | _CMD_CORE,	&tui_rmfile },
	{ 'T',	CMD_SAVETRK | _CMD_CURTRK | _CMD_CURTRK_REC | _CMD_CORE,	&tui_op_trk },
	{ 'd',	CMD_RM | _CMD_CURTRK | _CMD_CORE,	&tui_rmfile },
	{ 'h',	_CMD_F1,	&tui_help },
	{ 'i',	CMD_SHOWTAGS | _CMD_CURTRK | _CMD_CORE,	&tui_op_trk },
	{ 'm',	CMD_MUTE | _CMD_CURTRK | _CMD_CORE,	&tui_vol },
	{ 'n',	CMD_NEXT | _CMD_F1 | _CMD_CORE,	&tui_op },
	{ 'p',	CMD_PREV | _CMD_F1 | _CMD_CORE,	&tui_op },
	{ 'q',	CMD_QUIT | _CMD_F1 | _CMD_CORE,	&tui_op },
	{ 's',	CMD_STOP | _CMD_F1 | _CMD_CORE,	&tui_op },
	{ FFKEY_UP,	CMD_VOLUP | _CMD_CURTRK | _CMD_CORE,	&tui_vol },
	{ FFKEY_DOWN,	CMD_VOLDOWN | _CMD_CURTRK | _CMD_CORE,	&tui_vol },
	{ FFKEY_RIGHT,	CMD_SEEKRIGHT | _CMD_F3 | _CMD_CORE,	&tui_seek },
	{ FFKEY_LEFT,	CMD_SEEKLEFT | _CMD_F3 | _CMD_CORE,	&tui_seek },
};

static const struct key* key2cmd(int key)
{
	size_t i, start = 0;
	uint k = (key & ~FFKEY_MODMASK), n = FFCNT(hotkeys);
	while (start != n) {
		i = start + (n - start) / 2;
		if (k == hotkeys[i].key) {
			return &hotkeys[i];
		} else if (k < hotkeys[i].key)
			n = i;
		else
			start = i + 1;
	}
	return NULL;
}

struct corecmd {
	fftask tsk;
	const struct key *k;
	void *udata;
};

static void tui_help(uint cmd)
{
	char buf[4096];
	ssize_t n;
	char *fn;
	fffd f;

	if (NULL == (fn = core->getpath(FFSTR("help-tui.txt"))))
		return;

	f = fffile_open(fn, FFO_RDONLY | FFO_NOATIME);
	ffmem_free(fn);
	if (f == FF_BADFD)
		return;
	n = fffile_read(f, buf, sizeof(buf));
	fffile_close(f);
	if (n > 0)
		fffile_write(ffstdout, buf, n);
}

static void tui_corecmd(void *param)
{
	struct corecmd *c = param;

	if (c->k->cmd & _CMD_F1) {
		cmdfunc1 func1 = (void*)c->k->func;
		func1(c->k->cmd & CMD_MASK);

	} else if (c->k->cmd & _CMD_F3) {
		if (gt->curtrk == NULL)
			goto done;
		cmdfunc3 func3 = (void*)c->k->func;
		func3(gt->curtrk, c->k->cmd & CMD_MASK, c->udata);

	} else if (c->k->cmd & (_CMD_CURTRK | _CMD_CURTRK_REC)) {
		cmdfunc func = (void*)c->k->func;
		struct tui *t = NULL;
		if ((c->k->cmd & _CMD_CURTRK) && gt->curtrk != NULL)
			t = gt->curtrk;
		else if ((c->k->cmd & _CMD_CURTRK_REC) && gt->curtrk_rec != NULL)
			t = gt->curtrk_rec;
		if (t == NULL)
			goto done;
		func(t, c->k->cmd & CMD_MASK);
	}

done:
	ffmem_free(c);
}

static void tui_corecmd_add(const struct key *k, void *udata)
{
	struct corecmd *c = ffmem_tcalloc1(struct corecmd);
	if (c == NULL) {
		syserrlog(core, NULL, "tui", "alloc");
		return;
	}
	c->tsk.handler = &tui_corecmd;
	c->tsk.param = c;
	c->udata = udata;
	c->k = k;
	core->task(&c->tsk, FMED_TASK_POST);
}

static void tui_cmdread(void *param)
{
	ffstd_ev ev;
	int r;

	ffmem_tzero(&ev);

	for (;;) {
		r = ffstd_event(ffstdin, &ev);
		if (r == 0)
			break;
		else if (r < 0)
			break;

		uint key = ffstd_key(ev.data, &ev.datalen);

		const struct key *k = key2cmd(key);
		if (k == NULL) {
			dbglog(core, NULL, "tui", "unknown key seq %*xb"
				, (size_t)ev.datalen, ev.data);
			continue;
		}
		dbglog(core, NULL, "tui", "received command %u", k->cmd & CMD_MASK);

		if (k->cmd & _CMD_CORE) {
			void *udata = NULL;
			if (k->cmd & _CMD_F3)
				udata = (void*)(size_t)key;
			tui_corecmd_add(k, udata);

		} else if (k->cmd & _CMD_F1) {
			cmdfunc1 func1 = (void*)k->func;
			func1(k->cmd & ~_CMD_F1);
		}
	}
}
