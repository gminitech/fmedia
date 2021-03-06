/**
Copyright (c) 2016 Simon Zolin */

#include <FF/audio/pcm.h>
#include <FF/array.h>


typedef struct fmed_cmd {
	ffarr in_files; //char*[]
	fftask tsk_start;

	byte repeat_all;
	byte list_random;
	char *trackno;

	uint playdev_name;
	uint captdev_name;
	uint lbdev_name;

	struct {
	uint out_format;
	uint out_rate;
	byte out_channels;
	};

	byte rec;
	byte mix;
	byte tags;
	byte info;
	uint seek_time;
	uint until_time;
	uint split_time;
	uint prebuffer;
	float start_level; //dB
	float stop_level; //dB
	uint stop_level_time; //msec
	uint stop_level_mintime; //msec
	uint64 fseek;
	ffstr meta;
	ffarr2 include_files; //ffstr[]
	ffarr2 exclude_files; //ffstr[]

	float gain;
	byte volume;
	byte pcm_peaks;
	byte pcm_crc;
	byte dynanorm;

	float vorbis_qual;
	uint opus_brate;
	uint aac_qual;
	char *aac_profile;
	ushort mpeg_qual;
	byte flac_complevel;
	byte stream_copy;

	ffstr globcmd;
	char *globcmd_pipename;
	byte bground;
	byte bgchild;
	char *conf_fn;
	byte notui;
	byte gui;
	byte print_time;
	byte cue_gaps;

	ffstr outfn;
	byte overwrite;
	byte out_copy;
	byte preserve_date;
	byte parallel;

	ffstr dummy;

	uint until_plback_end :1;
} fmed_cmd;

static inline int cmd_init(fmed_cmd *cmd)
{
	cmd->vorbis_qual = -255;
	cmd->aac_qual = (uint)-1;
	cmd->mpeg_qual = 0xffff;
	cmd->flac_complevel = 0xff;

	cmd->lbdev_name = (uint)-1;
	cmd->volume = 100;
	cmd->cue_gaps = 255;
	return 0;
}

static inline void cmd_destroy(fmed_cmd *cmd)
{
	FFARR_FREE_ALL_PTR(&cmd->in_files, ffmem_free, char*);
	ffstr_free(&cmd->outfn);

	ffstr_free(&cmd->meta);
	ffmem_safefree(cmd->aac_profile);
	ffmem_safefree(cmd->trackno);
	ffmem_safefree(cmd->conf_fn);

	ffmem_safefree(cmd->globcmd_pipename);
	ffstr_free(&cmd->globcmd);
	ffarr2_free(&cmd->include_files);
	ffarr2_free(&cmd->exclude_files);
}
