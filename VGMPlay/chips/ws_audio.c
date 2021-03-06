
#include <stdlib.h>
#include <stddef.h>	// for NULL
#include <string.h>	// for memset
#include "mamedef.h"

typedef UINT8	BYTE;
typedef UINT8	byte;
typedef UINT8	uint8;

//#include <windows.h>
//#include "types.h"
//#include "./nec/necintrf.h"
//#include "ws_memory.h"
#include "ws_initialIo.h"
//#include "ws_io.h"
#include "ws_audio.h"
//#include "wsr_player.h"

#define SNDP	chip->ws_ioRam[0x80]
#define SNDV	chip->ws_ioRam[0x88]
#define SNDSWP	chip->ws_ioRam[0x8C]
#define SWPSTP	chip->ws_ioRam[0x8D]
#define NSCTL	chip->ws_ioRam[0x8E]
#define WAVDTP	chip->ws_ioRam[0x8F]
#define SNDMOD	chip->ws_ioRam[0x90]
#define SNDOUT	chip->ws_ioRam[0x91]
#define PCSRL	chip->ws_ioRam[0x92]
#define PCSRH	chip->ws_ioRam[0x93]
#define DMASL	chip->ws_ioRam[0x40]
#define DMASH	chip->ws_ioRam[0x41]
#define DMASB	chip->ws_ioRam[0x42]
#define DMADB	chip->ws_ioRam[0x43]
#define DMADL	chip->ws_ioRam[0x44]
#define DMADH	chip->ws_ioRam[0x45]
#define DMACL	chip->ws_ioRam[0x46]
#define DMACH	chip->ws_ioRam[0x47]
#define DMACTL	chip->ws_ioRam[0x48]
#define SDMASL	chip->ws_ioRam[0x4A]
#define SDMASH	chip->ws_ioRam[0x4B]
#define SDMASB	chip->ws_ioRam[0x4C]
#define SDMACL	chip->ws_ioRam[0x4E]
#define SDMACH	chip->ws_ioRam[0x4F]
#define SDMACTL	chip->ws_ioRam[0x52]

//SoundDMA �̓]���Ԋu
// ���ۂ̐��l��������Ȃ��̂ŁA�\�z�ł�
// �T���v�����O��������l���Ă݂Ĉȉ��̂悤�ɂ���
// 12KHz = 1.00HBlank = 256cycles�Ԋu
// 16KHz = 0.75HBlank = 192cycles�Ԋu
// 20KHz = 0.60HBlank = 154cycles�Ԋu
// 24KHz = 0.50HBlank = 128cycles�Ԋu
const int DMACycles[4] = { 256, 192, 154, 128 };

typedef struct
{
	int wave;
	int lvol;
	int rvol;
	long offset;
	long delta;
	long pos;
	UINT8 Muted;
} WS_AUDIO;

typedef struct
{
	WS_AUDIO ws_audio[4];
	int sweepDelta;
	int sweepOffset;
	int SweepTime;
	int SweepStep;
	int SweepCount;
	int SweepFreq;
	int NoiseType;
	int NoiseRng;
	int MainVolume;
	int PCMVolumeLeft;
	int PCMVolumeRight;
	//int WaveAdrs;

	UINT8	 ws_ioRam[0x100];
	UINT8	*ws_internalRam;
	
	int clock;
	int smplrate;
} wsa_state;

#define DEFAULT_CLOCK	3072000


static void ws_audio_process(wsa_state* chip);



extern UINT8 CHIP_SAMPLING_MODE;
extern INT32 CHIP_SAMPLE_RATE;
extern UINT32 SampleRate;

#define MAX_CHIPS	0x02
static wsa_state WSAData[MAX_CHIPS];

int ws_audio_init(UINT8 ChipID, int clock)
{
	wsa_state* chip;
	UINT8 CurChn;
	
	if (ChipID >= MAX_CHIPS)
		return 0;
	
	chip = &WSAData[ChipID];
	
	chip->ws_internalRam = (UINT8*)malloc(0x4000);	// actual size is 64 KB, but the audio chip can only access 16 KB
	
	chip->clock = clock;
	chip->smplrate = SampleRate;
	if (((CHIP_SAMPLING_MODE & 0x01) && chip->smplrate < CHIP_SAMPLE_RATE) ||
		CHIP_SAMPLING_MODE == 0x02)
		chip->smplrate = CHIP_SAMPLE_RATE;
	
	for (CurChn = 0; CurChn < 4; CurChn ++)
		chip->ws_audio[CurChn].Muted = 0x00;
	
	return chip->smplrate; 
}

void ws_audio_reset(UINT8 ChipID)
{
	wsa_state* chip = &WSAData[ChipID];
	int i;
	
	memset(&chip->ws_audio, 0, sizeof(WS_AUDIO));
	chip->SweepTime = 0;
	chip->SweepStep = 0;
	chip->NoiseType = 0;
	chip->NoiseRng = 1;
	chip->MainVolume = 0x02;	// 0x04
	chip->PCMVolumeLeft = 0;
	chip->PCMVolumeRight = 0;
	
	chip->sweepDelta = chip->clock * 256 / chip->smplrate;
	chip->sweepOffset = 0;
	
	for (i=0x80;i<0xc9;i++)
		ws_audio_port_write(ChipID, i, initialIoValue[i]);
}

void ws_audio_done(UINT8 ChipID)
{
	wsa_state* chip = &WSAData[ChipID];
	
	free(chip->ws_internalRam);
	chip->ws_internalRam = NULL;
	
	return;
}

void ws_audio_update(UINT8 ChipID, stream_sample_t** buffer, int length)
{
	wsa_state* chip = &WSAData[ChipID];
	stream_sample_t* bufL;
	stream_sample_t* bufR;
	int i, ch, cnt;
	long w, l, r;

	bufL = buffer[0];
	bufR = buffer[1];
	for (i=0; i<length; i++)
	{
		chip->sweepOffset += chip->sweepDelta;
		while(chip->sweepOffset >= 0x10000)
		{
			chip->sweepOffset -= 0x10000;
			ws_audio_process(chip);
		}
		l = r = 0;

		for (ch=0; ch<4; ch++)
		{
			if (chip->ws_audio[ch].Muted)
				continue;
			
			if ((ch==1) && (SNDMOD&0x20))
			{
				// Voice�o��
				w = chip->ws_ioRam[0x89];
				w -= 0x80;
				l += chip->PCMVolumeLeft  * w;
				r += chip->PCMVolumeRight * w;
			}
			else if (SNDMOD&(1<<ch))
			{
				if ((ch==3) && (SNDMOD&0x80))
				{
					//Noise

					//OSWAN�̋[�������̏����Ɠ����̂��
#define BIT(n) (1<<n)
					const unsigned long noise_mask[8] =
					{
						BIT(0)|BIT(1),
						BIT(0)|BIT(1)|BIT(4)|BIT(5),
						BIT(0)|BIT(1)|BIT(3)|BIT(4),
						BIT(0)|BIT(1)|BIT(4)|BIT(6),
						BIT(0)|BIT(2),
						BIT(0)|BIT(3),
						BIT(0)|BIT(4),
						BIT(0)|BIT(2)|BIT(3)|BIT(4)
					};

					const unsigned long noise_bit[8] =
					{
						BIT(15),
						BIT(14),
						BIT(13),
						BIT(12),
						BIT(11),
						BIT(10),
						BIT(9),
						BIT(8)
					};

					int Masked, XorReg;

					chip->ws_audio[ch].offset += chip->ws_audio[ch].delta;
					cnt = chip->ws_audio[ch].offset>>16;
					chip->ws_audio[ch].offset &= 0xffff;
					while (cnt > 0)
					{
						cnt--;

						chip->NoiseRng &= noise_bit[chip->NoiseType]-1;
						if (!chip->NoiseRng) chip->NoiseRng = noise_bit[chip->NoiseType]-1;

						Masked = chip->NoiseRng & noise_mask[chip->NoiseType];
						XorReg = 0;
						while (Masked)
						{
							XorReg ^= Masked&1;
							Masked >>= 1;
						}
						if (XorReg)
							chip->NoiseRng |= noise_bit[chip->NoiseType];
						chip->NoiseRng >>= 1;
					}

					PCSRL = (byte)(chip->NoiseRng&0xff);
					PCSRH = (byte)((chip->NoiseRng>>8)&0x7f);

					w = (chip->NoiseRng&1)? 0x7f:-0x80;
					l += chip->ws_audio[ch].lvol * w;
					r += chip->ws_audio[ch].rvol * w;
				}
				else
				{
					chip->ws_audio[ch].offset += chip->ws_audio[ch].delta;
					cnt = chip->ws_audio[ch].offset>>16;
					chip->ws_audio[ch].offset &= 0xffff;
					chip->ws_audio[ch].pos += cnt;
					chip->ws_audio[ch].pos &= 0x1f;
					w = chip->ws_internalRam[(chip->ws_audio[ch].wave&0xFFF0) + (chip->ws_audio[ch].pos>>1)];
					if ((chip->ws_audio[ch].pos&1) == 0)
						w = (w<<4)&0xf0;	//���ʃj�u��
					else
						w = w&0xf0;			//��ʃj�u��
					w -= 0x80;
					l += chip->ws_audio[ch].lvol * w;
					r += chip->ws_audio[ch].rvol * w;
				}
			}
		}

		/*l = l * chip->MainVolume;
		if (l > 0x7fff) l = 0x7fff;
		else if (l < -0x8000) l = -0x8000;
		r = r * chip->MainVolume;
		if (r > 0x7fff) r = 0x7fff;
		else if (r < -0x8000) r = -0x8000;

		*buffer++ = (short)l;
		*buffer++ = (short)r;*/
		bufL[i] = l * chip->MainVolume;
		bufR[i] = r * chip->MainVolume;
	}
}

void ws_audio_port_write(UINT8 ChipID, BYTE port, BYTE value)
{
	wsa_state* chip = &WSAData[ChipID];
	int i;
	long freq;

	//Update_SampleData();

	chip->ws_ioRam[port]=value;

	switch (port)
	{
	// 0x80-0x87�̎��g�����W�X�^�ɂ���
	// - ���b�N�}��&�t�H���e��0x0f�̋Ȃł́A���g��=0xFFFF �̉����s�v
	// - �f�W�����f�B�[�v���W�F�N�g��0x0d�̋Ȃ̃m�C�Y�� ���g��=0x07FF �ŉ���o��
	// ���܂�A0xFFFF �̎���������o���Ȃ����Ă��Ƃ��낤���B
	//   �ł�A0x07FF �̎������o���Ȃ����ǁA�m�C�Y��������o���̂���B
	case 0x80:
	case 0x81:	i=(((unsigned int)chip->ws_ioRam[0x81])<<8)+((unsigned int)chip->ws_ioRam[0x80]);
				if (i == 0xffff)
					freq = 0;
				else
					freq = chip->clock/(2048-(i&0x7ff));
				chip->ws_audio[0].delta = (long)((float)freq*(float)65536/(float)chip->smplrate);
				break;
	case 0x82:
	case 0x83:	i=(((unsigned int)chip->ws_ioRam[0x83])<<8)+((unsigned int)chip->ws_ioRam[0x82]);
				if (i == 0xffff)
					freq = 0;
				else
					freq = chip->clock/(2048-(i&0x7ff));
				chip->ws_audio[1].delta = (long)((float)freq*(float)65536/(float)chip->smplrate);
				break;
	case 0x84:
	case 0x85:	i=(((unsigned int)chip->ws_ioRam[0x85])<<8)+((unsigned int)chip->ws_ioRam[0x84]);
				chip->SweepFreq = i;
				if (i == 0xffff)
					freq = 0;
				else
					freq = chip->clock/(2048-(i&0x7ff));
				chip->ws_audio[2].delta = (long)((float)freq*(float)65536/(float)chip->smplrate);
				break;
	case 0x86:
	case 0x87:	i=(((unsigned int)chip->ws_ioRam[0x87])<<8)+((unsigned int)chip->ws_ioRam[0x86]);
				if (i == 0xffff)
					freq = 0;
				else
					freq = chip->clock/(2048-(i&0x7ff));
				chip->ws_audio[3].delta = (long)((float)freq*(float)65536/(float)chip->smplrate);
				break;
	case 0x88:
				chip->ws_audio[0].lvol = (value>>4)&0xf;
				chip->ws_audio[0].rvol = value&0xf;
				break;
	case 0x89:
				chip->ws_audio[1].lvol = (value>>4)&0xf;
				chip->ws_audio[1].rvol = value&0xf;
				break;
	case 0x8A:
				chip->ws_audio[2].lvol = (value>>4)&0xf;
				chip->ws_audio[2].rvol = value&0xf;
				break;
	case 0x8B:
				chip->ws_audio[3].lvol = (value>>4)&0xf;
				chip->ws_audio[3].rvol = value&0xf;
				break;
	case 0x8C:
				chip->SweepStep = (signed char)value;
				break;
	case 0x8D:
				//Sweep�̊Ԋu�� 1/375[s] = 2.666..[ms]
				//CPU Clock�Ō����� 3072000/375 = 8192[cycles]
				//�����̐ݒ�l��n�Ƃ���ƁA8192[cycles]*(n+1) �Ԋu��Sweep���邱�ƂɂȂ�
				//
				//����� HBlank (256cycles) �̊Ԋu�Ō����ƁA
				//�@8192/256 = 32
				//�Ȃ̂ŁA32[HBlank]*(n+1) �Ԋu�ƂȂ�
				chip->SweepTime = (((unsigned int)value)+1)<<5;
				chip->SweepCount = chip->SweepTime;
				break;
	case 0x8E:
				chip->NoiseType = value&7;
				if (value&8) chip->NoiseRng = 1;	//�m�C�Y�J�E���^�[���Z�b�g
				break;
	case 0x8F:
				//WaveAdrs = (unsigned int)value<<6;
				chip->ws_audio[0].wave = (unsigned int)value<<6;
				chip->ws_audio[1].wave = chip->ws_audio[0].wave + 0x10;
				chip->ws_audio[2].wave = chip->ws_audio[1].wave + 0x10;
				chip->ws_audio[3].wave = chip->ws_audio[2].wave + 0x10;
				break;
	case 0x90:
				break;
	case 0x91:
				//�����ł̃{�����[�������́A���Speaker�ɑ΂��Ă̒��������炵���̂ŁA
				//�w�b�h�t�H���ڑ�����Ă���ƔF��������Ζ�薳���炵���B
				chip->ws_ioRam[port] |= 0x80;
				break;
	case 0x92:
	case 0x93:
				break;
	case 0x94:
				chip->PCMVolumeLeft  = (value&0xc)*2;
				chip->PCMVolumeRight = ((value<<2)&0xc)*2;
				break;
	/*case 0x52:
				if (value&0x80)
					ws_timer_set(2, DMACycles[value&3]);
				break;*/
	}
}

BYTE ws_audio_port_read(UINT8 ChipID, BYTE port)
{
	wsa_state* chip = &WSAData[ChipID];
	return (chip->ws_ioRam[port]);
}

// HBlank�Ԋu�ŌĂ΂��
// Note: Must be called every 256 cycles (3072000 Hz clock), i.e. at 12000 Hz
static void ws_audio_process(wsa_state* chip)
{
	long freq;

	if (chip->SweepStep && (SNDMOD&0x40))
	{
		if (chip->SweepCount < 0)
		{
			chip->SweepCount = chip->SweepTime;
			chip->SweepFreq += chip->SweepStep;
			chip->SweepFreq &= 0x7FF;

			//Update_SampleData();

			freq = chip->clock/(2048-chip->SweepFreq);
			chip->ws_audio[2].delta = (long)((float)freq*(float)65536/(float)chip->smplrate);
		}
		chip->SweepCount--;
	}
}

/*void ws_audio_sounddma(void)
{
	int i, j, b;

	if ((SDMACTL&0x88)==0x80)
	{
		i=(SDMACH<<8)|SDMACL;
		j=(SDMASB<<16)|(SDMASH<<8)|SDMASL;
		b=cpu_readmem20(j);

		Update_SampleData();

		ws_ioRam[0x89]=b;
		i--;
		j++;
		if(i<32)
		{
			i=0;
			SDMACTL&=0x7F;
		}
		else
		{
			ws_timer_set(2, DMACycles[SDMACTL&3]);
		}
		SDMASB=(byte)((j>>16)&0xFF);
		SDMASH=(byte)((j>>8)&0xFF);
		SDMASL=(byte)(j&0xFF);
		SDMACH=(byte)((i>>8)&0xFF);
		SDMACL=(byte)(i&0xFF);
	}
}*/

void ws_write_ram(UINT8 ChipID, UINT16 offset, UINT8 value)
{
	wsa_state* chip = &WSAData[ChipID];
	
	// RAM - 16 KB (WS) / 64 KB (WSC) internal RAM
	if (offset >= 0x4000)
		return;	// We only allocated 16 KB.
	
	chip->ws_internalRam[offset] = value;
	return;
}

void ws_set_mute_mask(UINT8 ChipID, UINT32 MuteMask)
{
	wsa_state* chip = &WSAData[ChipID];
	UINT8 CurChn;
	
	for (CurChn = 0; CurChn < 4; CurChn ++)
		chip->ws_audio[CurChn].Muted = (MuteMask >> CurChn) & 0x01;
	
	return;
}
