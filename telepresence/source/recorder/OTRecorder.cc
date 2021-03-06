/*
* Copyright (C) 2013 Mamadou DIOP
* Copyright (C) 2013 Doubango Telecom <http://www.doubango.org>
* License: GPLv3
* This file is part of the open source SIP TelePresence system <https://code.google.com/p/telepresence/>
*/
#include "opentelepresence/recorder/OTRecorder.h"
#include "opentelepresence/recorder/CryptoWrapperAPI.h"
#include "opentelepresence/OTMutex.h"

#include "tsk_memory.h"
#include "tsk_time.h"

#include <assert.h>

#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sstream>


extern "C"
{
	#include <libavformat/avformat.h>
	#include <libswresample/swresample.h>
	#include <libavutil/opt.h>
}

#if !defined(OT_RECORDER_USE_TSK_TIME)
#	define OT_RECORDER_USE_TSK_TIME 1
#endif


#define FRAME_BUFFER_SIZE (40980 * 10) //400K


#if OT_RECORDER_USE_TSK_TIME
static const AVRational kRecorderBaseTime = {1, 1000}; /* to be used with tsk_time_now() */
#define ot_gettime tsk_time_now
#else
static const AVRational kRecorderBaseTime = {1, 1000000}; /* to be used with av_gettime() */
#define ot_gettime av_gettime
#endif

static const char* ot_av_err2str(int errnum)
{
	static char __errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    return av_make_error_string(__errbuf, AV_ERROR_MAX_STRING_SIZE, errnum);
}

class OTAudioResamplerFFmpeg : public OTObject
{
public:
	OTAudioResamplerFFmpeg(
			struct AVCodecContext* ctx, 
			enum AVSampleFormat eInSampleFormat,
			int32_t nInChannelsCount,
			int32_t nInSampleRate
		)
		: m_eOutSampleFormat(ctx->sample_fmt)
		, m_eInSampleFormat(eInSampleFormat)
		, m_pSwr(NULL)

		, m_nLastInBufferSizeInBytes(0)
		, m_nOutBufferSizeInBytes(0)
		, m_nOutBufferSizeInSamples(0)
		, m_pOutBufferPtr(NULL)
	{
		OT_DEBUG_INFO_EX(kOTMobuleNameFFmpegRecorder, "Create audio resampler %d->%d", m_eInSampleFormat, m_eOutSampleFormat);
		m_nOutBytesPerSample = av_get_bytes_per_sample(m_eOutSampleFormat);
		m_nInBytesPerSample = av_get_bytes_per_sample(m_eInSampleFormat);
		if((m_pSwr = swr_alloc()))
		{
			int ret = av_opt_set_int(m_pSwr, "in_channel_layout",  av_get_default_channel_layout(nInChannelsCount), 0);
			ret = av_opt_set_int(m_pSwr, "out_channel_layout", ctx->channel_layout,  0);
			ret = av_opt_set_int(m_pSwr, "in_sample_rate",     nInSampleRate, 0);
			ret = av_opt_set_int(m_pSwr, "out_sample_rate",    ctx->sample_rate, 0);
			ret = av_opt_set_sample_fmt(m_pSwr, "in_sample_fmt",  m_eInSampleFormat, 0);
			ret = av_opt_set_sample_fmt(m_pSwr, "out_sample_fmt", m_eOutSampleFormat,  0);
			ret = swr_init(m_pSwr);
			if(ret < 0)
			{
				OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "swr_init(%d->%d) failed: %s", m_eInSampleFormat, m_eOutSampleFormat, ot_av_err2str(ret));
				_deInit();
			}
		}
		else
		{
			OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Failed to allocate SwrContext");
		}
	}
	virtual ~OTAudioResamplerFFmpeg()
	{
		_deInit();
		OT_DEBUG_INFO("*** OTAudioResamplerFFmpeg destroyed ***");
	}
	virtual OT_INLINE const char* getObjectId() { return "OTAudioResamplerFFmpeg"; }
	virtual OT_INLINE bool isValid(){ return (m_pSwr != NULL); }
	virtual OT_INLINE const void* getBufferPtr(){ return m_pOutBufferPtr; }
	virtual OT_INLINE size_t getBufferSizeInBytes(){ return m_nOutBufferSizeInBytes; }
	virtual OT_INLINE size_t getBufferSizeInSamples(){ return m_nOutBufferSizeInSamples; }

	// returns number of bytes
	virtual OT_INLINE size_t resample(const void* pInBufferPtr, size_t nInBufferSizeInBytes)
	{
		if(!isValid())
		{
			OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Not valid");
			return 0;
		}

		if(m_nLastInBufferSizeInBytes != nInBufferSizeInBytes)
		{
			size_t nInBufferSizeInSamples = (nInBufferSizeInBytes / m_nInBytesPerSample);
			size_t nOutBufferSizeInBytes = (nInBufferSizeInSamples * m_nOutBytesPerSample); 
			if(m_nOutBufferSizeInBytes < nOutBufferSizeInBytes)
			{
				if(!(m_pOutBufferPtr = tsk_realloc(m_pOutBufferPtr, nOutBufferSizeInBytes)))
				{
					m_nOutBufferSizeInBytes = 0;
					m_nOutBufferSizeInSamples = 0;
					return 0;
				}
				m_nOutBufferSizeInBytes = nOutBufferSizeInBytes;
				m_nOutBufferSizeInSamples = nInBufferSizeInSamples;
			}
			m_nLastInBufferSizeInBytes = nInBufferSizeInBytes;
		}

		int ret = swr_convert(m_pSwr, (uint8_t **)&m_pOutBufferPtr, m_nOutBufferSizeInSamples, (const uint8_t **)&pInBufferPtr, m_nOutBufferSizeInSamples);
		if(ret < 0)
		{
			OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "swr_convert(%d->%d) failed: %s", m_eInSampleFormat, m_eOutSampleFormat, ot_av_err2str(ret));
			return 0;
		}

		return ((size_t)ret * m_nOutBytesPerSample);
	}

private:
	void _deInit()
	{
		if(m_pSwr)
		{
			swr_free(&m_pSwr);
			m_pSwr = NULL;
		}
		TSK_FREE(m_pOutBufferPtr);
		m_nOutBufferSizeInBytes = m_nOutBufferSizeInSamples = 0;
	}

private:
	enum AVSampleFormat m_eOutSampleFormat, m_eInSampleFormat;
	size_t m_nOutBytesPerSample, m_nInBytesPerSample;
	SwrContext *m_pSwr;
	size_t m_nLastInBufferSizeInBytes;
	size_t m_nOutBufferSizeInBytes, m_nOutBufferSizeInSamples;
	void* m_pOutBufferPtr;
};

class OTRecorderFFmpeg : public OTRecorder
{
public:
	OTRecorderFFmpeg(bool isEncryption,std::string encryptionKey, std::string Sm2PublicKey, 
											std::string strRecordOpenFireServlet, 
											std::string strRecordFile,
											std::string strConferenceId,
											std::string strFilePath, OTMediaType_t eMediaType);
	virtual ~OTRecorderFFmpeg();
	virtual OT_INLINE const char* getObjectId() { return "OTRecorderFFmpeg"; }

	// OTRecorder overrides 
	bool setVideoParams(uint32_t nVideoWidth, uint32_t nVideoHeight, uint32_t nMotionRank, uint32_t nGopSizeInSec, uint32_t nFps);
	bool setAudioParams(uint32_t nPtime, uint32_t nRate, uint32_t nChannels);
	bool open(OTMediaType_t eMediaType);
	bool writeRtpVideoPayload(const void* rtpPayPtr, size_t rtpPaySize){ OT_ASSERT(false); return false; };
	bool writeRtpAudioPayload(const void* rtpPayPtr, size_t rtpPaySize){ OT_ASSERT(false); return false; };
	bool writeRawVideoPayload(const void* yuv420PayPtr, size_t yuv420PaySize);
	bool writeRawAudioPayload(const void* pcmPayPtr, size_t pcmPaySize);
	bool close(OTMediaType_t eMediaType);
	int HttpPost(const std::string & strUrl, const std::string & strMeetingNO, const std::string & Path , std::string & strResponse);
	int HttpGet(const std::string & strUrl, const std::string & strRecordId, const std::string & strMeetingNO, std::string & strResponse);


private:
	// Private functions
	AVStream* _addStream(AVCodec **ppCodec, enum AVCodecID eCodecId);
	bool _openAudio();
	bool _closeAudio();
	bool _openVideo();
	bool _closeVideo();
	int PutVideoDataSM4ToStream(uint8_t * buffer, int size, int stream_index, int flags,int64_t pts, int64_t dts);
	int PutAudioDataSM4ToStream(uint8_t * buffer, int size, int stream_index, int flags,int64_t pts, int64_t dts);

private:
	bool m_bVideoOpened, m_bAudioOpened;
	AVFormatContext *m_pFormatCtx;
    AVStream *m_pAudioStream, *m_pVideoStream;
    AVCodec *m_pAudioCodec, *m_pVideoCodec;
	AVFrame *m_pVideoFrame, *m_pAudioFrame;
	OTObjectWrapper<OTAudioResamplerFFmpeg *> m_oAudioResampler;
    double m_dAudioPts, m_dVideoPts; 
	uint8_t* m_pAVAudioBufferPtr;
	size_t m_nAVAudioBufferSizeInBytes, m_nAVAudioBufferSizeInSamples, m_nAVAudioBufferIndexInBytes;
	uint64_t m_nFirstWriteTime;
	OTObjectWrapper<OTMutex *> m_oMutex;
	uint8_t *Sm4Buffer;
	int Sm4Buffer_len;
	FILE * fp_clzhan;

};

class OTRecorderWebM : public OTRecorder
{
public:
	OTRecorderWebM( bool isEncryption,std::string encryptionKey, std::string Sm2PublicKey,
																std::string strRecordOpenFireServlet, 
																std::string strRecordFile,
																std::string strConferenceId,
									std::string strFilePath, OTMediaType_t eMediaType);
	virtual ~OTRecorderWebM();
	virtual OT_INLINE const char* getObjectId() { return "OTRecorderWebM"; }

	// OTRecorder overrides 
	bool setVideoParams(uint32_t nWidth, uint32_t nVideoHeight, uint32_t nFps);
	bool setAudioParams(uint32_t nPtime, uint32_t nRate, uint32_t nChannels){ OT_ASSERT(false); return false; };
	bool open(OTMediaType_t eMediaType);
	bool writeRtpVideoPayload(const void* rtpPayPtr, size_t rtpPaySize);
	bool writeRtpAudioPayload(const void* rtpPayPtr, size_t rtpPaySize);
	bool writeRawVideoPayload(const void* yuv420PayPtr, size_t yuv420PaySize){ OT_ASSERT(false); return false; };
	bool writeRawAudioPayload(const void* pcmPayPtr, size_t pcmPaySize){ OT_ASSERT(false); return false; };
	bool close(OTMediaType_t eMediaType);

private:
	FILE* m_pFile;
	struct EbmlGlobal* m_pGlob;

	struct EbmlRational m_fps;
	
	uint32_t m_nVideoWidth;
	uint32_t m_nVideoHeight;

	uint32_t m_nPts;

	struct{
		uint8_t* Ptr;
		size_t Size;
		size_t index;
	} m_Buffer;
};

//
//	OTRecorder
//

OTRecorder::OTRecorder(bool isEncryption,std::string encryptionKey, std::string Sm2PublicKey,
											std::string strRecordOpenFireServlet, 
											std::string strRecordFile,
											std::string strConferenceId,
											std::string strFilePath, OTMediaType_t eMediaType)
: isEncryption(isEncryption)
, encryptionKey(encryptionKey)
, Sm2PublicKey(Sm2PublicKey)
, m_strRecordOpenFireServlet(strRecordOpenFireServlet)
, m_strRecordFile(strRecordFile)
, m_strConferenceId(strConferenceId)
, m_strFilePath(strFilePath)
, m_eMediaType(eMediaType)
{

}

OTRecorder::~OTRecorder()
{
	OT_DEBUG_INFO("*** OTRecorder(%s) destroyed ***", getObjectId());
}

OTObjectWrapper<OTRecorder*> OTRecorder::New(bool isEncryption,std::string encryptionKey, std::string Sm2PublicKey, 
																std::string strRecordOpenFireServlet, 
																std::string strRecordFile,
																std::string strConferenceId,
																std::string strFilePath, OTMediaType_t eMediaType)
{
#if 1
	return new OTRecorderFFmpeg(isEncryption, encryptionKey, Sm2PublicKey,strRecordOpenFireServlet,strRecordFile,strConferenceId, strFilePath, eMediaType);
#elif 1
	//return new OTRecorderWebM(isEncryption, encryptionKey, Sm2PublicKey, strFilePath, eMediaType);
#else
	return NULL;
#endif
}


//
//	OTRecorderFFmpeg
//

static size_t OnWriteData(void* buffer, size_t size, size_t nmemb, void* lpVoid)
{
	std::string* str = dynamic_cast<std::string*>((std::string *)lpVoid);
	if( NULL == str || NULL == buffer )
	{
		return -1;
	}

	char* pData = (char*)buffer;
	str->append(pData, size * nmemb);
	return nmemb;
} 
static int str_to_hex(unsigned char *string, const int len , unsigned char *cbuf, int & cbuf_len)
{

	unsigned char high;
	unsigned char low;
	int idx, ii=0;
	for (idx=0; idx<len; idx+=2)
	{
		high = string[idx];
		low = string[idx+1];

		if(high>='0' && high<='9')
			high = high-'0';
		else if(high>='A' && high<='F')
			high = high - 'A' + 10;
		else if(high>='a' && high<='f')
			high = high - 'a' + 10;
		else
			return -1;

		if(low>='0' && low<='9')
			low = low-'0';
		else if(low>='A' && low<='F')
			low = low - 'A' + 10;
		else if(low>='a' && low<='f')
			low = low - 'a' + 10;
		else
			return -1;

		cbuf[ii++] = high<<4 | low;
	}
	return 0;
}

static std::string int2str(int i) {

	    std::stringstream ss;
		    ss << i;
			    return ss.str();

}
static int create_multi_dir(const char *path)
{
	int i, len;
	len = strlen(path);
	char dir_path[len+1];
	dir_path[len] = '\0';

	strncpy(dir_path, path, len);

	for (i=0; i<len; i++)
	{
		if (dir_path[i] == '/' && i > 0)
		{
			dir_path[i]='\0';
			if (access(dir_path, F_OK) < 0)
			{
				if (mkdir(dir_path, 0755) < 0)
				{
					printf("mkdir=%s:msg=%s\n", dir_path, strerror(errno));
					return -1;
				}
			}
			dir_path[i]='/';
		}
	}

	return 0;
}





int OTRecorderFFmpeg::HttpPost(const std::string & strUrl, const std::string & strMeetingNO, const std::string & Path , std::string & strResponse)
{

	CURL *curl;
	CURLcode res;

	curl = curl_easy_init();
	if (curl)
	{   
		std::string strparam = "from=RECODER&MeetingNo=" + strMeetingNO;
		strparam += "&";
		strparam += "isok=OK&path=";
		strparam += Path;
		strparam += "&msg= This is recorder ok!" ;


		printf("strparam = %s\n", strparam.c_str());

		curl_easy_setopt(curl, CURLOPT_URL,  strUrl.c_str());   // 指定url

		curl_easy_setopt(curl, CURLOPT_POST, 1); 

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strparam.c_str());    // 指定post内容

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWriteData);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&strResponse);
		res = curl_easy_perform(curl);
		curl_easy_cleanup(curl);


		printf(" post strResponse   = %s\n", strResponse.data());
	}   
	else
	{   
		return -1; 
	}   
	return 0;

}
int OTRecorderFFmpeg::HttpGet(const std::string & strUrl, const std::string & strRecordId, const std::string & strMeetingNO, std::string & strResponse)
{
	CURL *curl;
	CURLcode res;

	curl = curl_easy_init();
	if (curl)
	{   
		std::string strparam1 = "RECODEID:" + strRecordId;
		std::string strparam2 = "MeetingNO:" + strMeetingNO;


		printf("strUrl = %s\n", strUrl.c_str());
		printf("strRecordId = %s\n",strRecordId.c_str());
		printf("strMeetingNO = %s\n", strMeetingNO.c_str());

		curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());
		struct curl_slist *headers=NULL; /* init to NULL is important */
		headers = curl_slist_append(headers, strparam1.c_str());
		headers = curl_slist_append(headers, strparam2.c_str());
		/* pass our list of custom made headers */
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

		curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWriteData);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&strResponse);
		res = curl_easy_perform(curl);

		curl_slist_free_all(headers); /* free the header list */
		curl_easy_cleanup(curl);

		printf("get strResponse   = %s\n", strResponse.data());
	}
	else
	{
		return -1;
	}

	return 0;
}



OTRecorderFFmpeg::OTRecorderFFmpeg(bool isEncryption,std::string encryptionKey, std::string Sm2PublicKey,
																std::string strRecordOpenFireServlet,
																std::string strRecordFile,
																std::string strConferenceId,
																std::string strFilePath,
																OTMediaType_t eMediaType)
: OTRecorder(isEncryption, encryptionKey, Sm2PublicKey, strRecordOpenFireServlet, strRecordFile, strConferenceId, strFilePath, eMediaType)
, m_bVideoOpened(false)
, m_bAudioOpened(false)
, m_pFormatCtx(NULL)
, m_pAudioStream(NULL)
, m_pVideoStream(NULL)
, m_pAudioCodec(NULL)
, m_pVideoCodec(NULL)
, m_pVideoFrame(NULL)
, m_pAudioFrame(NULL)
, m_dAudioPts(0.0)
, m_dVideoPts(0.0)
, m_pAVAudioBufferPtr(NULL)
, m_nAVAudioBufferSizeInBytes(0)
, m_nAVAudioBufferSizeInSamples(0)
, m_nAVAudioBufferIndexInBytes(0)
, m_nFirstWriteTime(0)
{
	int ret;

	//get
	// get 120100's sm4key and sm2key or use default key

	//HttpGet(const std::string & strUrl, const std::string & strRecordId, const std::string & strMeetingNO, std::string & strResponse)

	std::string m_strResponse;
	std::string sm4_key;
	std::string sm2_public_key;
	
	if(isEncryption)
	{
		if(!m_strRecordOpenFireServlet.empty())
		{
			if(0 != HttpGet(m_strRecordOpenFireServlet, strRecordFile, strConferenceId, m_strResponse))
			{
				printf("%s:%d error %s\n",__func__,__LINE__,__FILE__);
				exit(0);
			}
		}


		if(!m_strResponse.empty())
		{
			const char * split = ";";
			char * p = NULL;
			p = strtok ((char *)m_strResponse.c_str(),split);
			while(p!=NULL)
			{

				if(strstr(p, "key1:") != NULL)
				{
					sm2_public_key += (p+5);
				}
				if(strstr(p, "key2:") != NULL)
				{
					sm4_key += (p+5);
				}
				p = strtok(NULL,split);
			}

			printf("sm4_key:%s len: %d\n",sm4_key.c_str(), sm4_key.size());
			printf("sm2_public_key : %s  len: %d\n",sm2_public_key.c_str(), sm2_public_key.size());
		}
		//TODO
#if 0
		if(!sm4_key.empty() && 16 == sm4_key.size())
		{
			encryptionKey = sm4_key;
		}
		if(!sm2_public_key.empty())
		{
			Sm2PublicKey = sm2_public_key;
		}
#endif
	}


	// Create mutex
	m_oMutex = new OTMutex();
	if(!m_oMutex)
	{
		return;
	}
	
	fp_clzhan = fopen("./1.txt", "w+");

	
	if(!(Sm4Buffer = (uint8_t*)av_malloc(FRAME_BUFFER_SIZE)))
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Failed to allocate buffer with size = %d", FRAME_BUFFER_SIZE);
		return;
	}
	Sm4Buffer_len  = FRAME_BUFFER_SIZE;



	// Create FFmpeg format context
	ret = avformat_alloc_output_context2(&m_pFormatCtx, NULL, NULL, strFilePath.c_str());
	if(!m_pFormatCtx || ret < 0)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Could not create context based on file extension: %s", ot_av_err2str(ret));
		close(m_eMediaType);
		return;
	}
	// Add audio stream
	if((eMediaType & OTMediaType_Audio) && m_pFormatCtx->oformat->audio_codec != AV_CODEC_ID_NONE)
	{
		if(!(m_pAudioStream = _addStream(&m_pAudioCodec, m_pFormatCtx->oformat->audio_codec)))
		{
			close(m_eMediaType);
			return;
		}
	}
	// Add video stream
	if((eMediaType & OTMediaType_Video) && m_pFormatCtx->oformat->video_codec != AV_CODEC_ID_NONE)
	{
		if(!(m_pVideoStream = _addStream(&m_pVideoCodec, m_pFormatCtx->oformat->video_codec)))
		{
			close(m_eMediaType);
			return;
		}
	}
}

OTRecorderFFmpeg::~OTRecorderFFmpeg()
{
	close(m_eMediaType);
}

bool OTRecorderFFmpeg::setVideoParams(uint32_t nVideoWidth, uint32_t nVideoHeight, uint32_t nMotionRank, uint32_t nGopSizeInSec, uint32_t nFps)
{
	if(!m_pFormatCtx)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "No valid context could be found");
		return false;
	}
	if(!m_pVideoStream)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "No valid video stream could be found");
		return false;
	}

	m_pVideoStream->codec->width = nVideoWidth;
	m_pVideoStream->codec->height = nVideoHeight;
	m_pVideoStream->codec->time_base.den = nFps;
    m_pVideoStream->codec->time_base.num = 1;
	m_pVideoStream->codec->gop_size = (nGopSizeInSec * nFps);
	m_pVideoStream->codec->bit_rate = (int)(nVideoWidth * nVideoHeight * nFps * nMotionRank * 0.07); /* bps */
	
	OT_DEBUG_INFO_EX(kOTMobuleNameFFmpegRecorder, "Recording video bitrate=%d", m_pVideoStream->codec->bit_rate);

	return true;
}

bool OTRecorderFFmpeg::setAudioParams(uint32_t nPtime, uint32_t nRate, uint32_t nChannels)
{
	if(!m_pFormatCtx)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "No valid context could be found");
		return false;
	}
	if(!m_pAudioStream)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "No valid audio stream could be found");
		return false;
	}

	(nPtime);
	
	switch(m_pAudioStream->codec->codec_id)
	{
		case AV_CODEC_ID_AAC:
			m_pAudioStream->codec->bit_rate = ((6144 * nRate * nChannels) / 1024);
			break;
		default:
			break;
	}
    m_pAudioStream->codec->sample_rate = nRate;
    m_pAudioStream->codec->channels = nChannels;
	m_pAudioStream->codec->channel_layout = av_get_default_channel_layout(m_pAudioStream->codec->channels);

	OT_DEBUG_INFO_EX(kOTMobuleNameFFmpegRecorder, "Audio bit rate = %d", m_pAudioStream->codec->bit_rate);
	
	return true;
}

bool OTRecorderFFmpeg::open(OTMediaType_t eMediaType)
{
	bool bRet = true, bFirstTime;
	int ret;

	m_oMutex->lock();

	if(!m_pFormatCtx)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "No valid context could be found)");
		bRet = false;
		goto bail;
	}

	bFirstTime = !(m_bAudioOpened || m_bVideoOpened);
	if(eMediaType & OTMediaType_Audio)
	{
		if(!_openAudio())
		{
			bRet = false;
			goto bail;
		}
	}

	if(eMediaType & OTMediaType_Video)
	{
		if(!_openVideo())
		{
			bRet = false;
			goto bail;
		}
	}

	if(bFirstTime)
	{
		/* open the output file, if needed */
		if (!(m_pFormatCtx->oformat->flags & AVFMT_NOFILE))
		{
			int ret = avio_open(&m_pFormatCtx->pb, m_strFilePath.c_str(), AVIO_FLAG_WRITE);
			if(ret < 0)
			{
				close(m_eMediaType);
				OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Could not open '%s': %s", m_strFilePath.c_str(), ot_av_err2str(ret));
				bRet = false;
				goto bail;
			}
		}

		av_dump_format(m_pFormatCtx, 0, m_strFilePath.c_str(), 1);

		/* Write the stream header, if any. */
		ret = avformat_write_header(m_pFormatCtx, NULL);
		if(ret < 0)
		{
			close(m_eMediaType);
			OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Error occurred when opening output file: %s", ot_av_err2str(ret));
			bRet = false;
			goto bail;
		}
		
		m_nFirstWriteTime = ot_gettime();
	}

bail:
	m_oMutex->unlock();

	return bRet;
}

int OTRecorderFFmpeg::PutVideoDataSM4ToStream(uint8_t * buffer, int size, int stream_index, int flags, int64_t pts, int64_t dts)
{
	printf("write Video Data size = %d\n", size);
		
	int ret = 0;
	AVPacket pkt = {0};
		
	av_init_packet(&pkt);


	//fprintf(fp_clzhan, "%d\n ", size);

	//for(int i = 0; i < size; i ++)
	//{    
		////printf("%02x ", pkt.data[i]);
		//fprintf(fp_clzhan, "%02x ", buffer[i]);
	//}
	////printf("\n\n");
	//fprintf(fp_clzhan,"\n\n");

	//fprintf(fp_clzhan, "org: %d\n ", size);

	//for(int i = 0; i < size; i ++)
	//{    
		////printf("%02x ", pkt.data[i]);
		//fprintf(fp_clzhan, "%02x ", buffer[i]);
	//}
	////printf("\n\n");
	//fprintf(fp_clzhan,"\n\n");

	if(size > 8)
	{
		if(buffer[0] == 0x00 && buffer[1] == 0x00 && buffer[2] == 0x01)
		{

			//fprintf(fp_clzhan, "%d\n ", size);
			//for(int i = 0; i < size ; i ++)
			//{    
				////printf("%02x ", pkt.data[i]);
				//fprintf(fp_clzhan, "%02x ", buffer[i]);
			//}
			////printf("\n\n");
			//fprintf(fp_clzhan,"\n\n");

			//fwrite(buffer, 1, size, fp_clzhan);
			size_t encrypt_output_len = 0;

			memset(Sm4Buffer, 0, Sm4Buffer_len);

			//unsigned char key[] = "01234567890abcef";
			if (0 != EncryptSM4((unsigned char*)encryptionKey.data(), buffer + 7, size - 7, Sm4Buffer + 7, &encrypt_output_len))
			{   
				OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "EncryptSM4 error, buff_password:%s\n", encryptionKey.data());
				exit(0);
				return -1;
			}  
			for(int i =0; i < 7;i++)
			{
				Sm4Buffer[i] = buffer[i];
			}
			pkt.size = (int)encrypt_output_len + 7;


			for(int i = 7; (i + 4) < pkt.size; i ++)
			{
				if(Sm4Buffer[i] == 0x00  && Sm4Buffer[i + 1] == 0x00 && Sm4Buffer[i + 2] == 0x01)
				{
					Sm4Buffer[i] = 0x1F;
					Sm4Buffer[i + 1] = 0x2F;
					Sm4Buffer[i + 2] = 0x3F;
					i = i + 3;
				}
				else if(Sm4Buffer[i] == 0x00  && Sm4Buffer[i + 1] == 0x00 && Sm4Buffer[i + 2] == 0x00 && Sm4Buffer[i + 3] == 0x01)
				{
					Sm4Buffer[i] = 0x1F;
					Sm4Buffer[i + 1] = 0x2F;
					Sm4Buffer[i + 2] = 0x3F;
					Sm4Buffer[i + 3] = 0x4F;
					i = i + 4;
				}
			}

			pkt.pts = pts;
			pkt.dts =dts; 

			pkt.stream_index = stream_index;
			pkt.data = Sm4Buffer;

			//pkt.size = (int)encrypt_output_len + 7;
			pkt.flags  = flags;

			fprintf(fp_clzhan, "size %d\n ", pkt.size);
			for(int i = 0; i < pkt.size; i ++)
			{    
				//printf("%02x ", pkt.data[i]);
				fprintf(fp_clzhan, "%02x ", pkt.data[i]);
			}
			//printf("\n\n");
			fprintf(fp_clzhan,"\n\n");



		}
		else if(buffer[0] == 0x00 && buffer[1] == 0x00 && buffer[2] == 0x00 && buffer[3] == 0x01)
		{

			//fprintf(fp_clzhan, "%d\n ", size);
			//for(int i = 0; i < size ; i ++)
			//{    
				////printf("%02x ", pkt.data[i]);
				//fprintf(fp_clzhan, "%02x ", buffer[i]);
			//}
			////printf("\n\n");
			//fprintf(fp_clzhan,"\n\n");

			//fwrite(buffer, 1, size, fp_clzhan);

			size_t encrypt_output_len = 0;

			memset(Sm4Buffer, 0, Sm4Buffer_len);

			//unsigned char key[] = "01234567890abcef";
			if (0 != EncryptSM4((unsigned char*)encryptionKey.data(), buffer + 8, size - 8, Sm4Buffer + 8, &encrypt_output_len))
			{   
				OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "EncryptSM4 error, buff_password:%s\n", encryptionKey.data());
				exit(0);
				return -1;
			}  
			for(int i =0; i < 8; i++ )
			{
				Sm4Buffer[i] = buffer[i];
			}

			pkt.size = (int)encrypt_output_len + 8;

			for(int i = 8; (i + 4) < pkt.size; i ++)
			{
				if(Sm4Buffer[i] == 0x00  && Sm4Buffer[i + 1] == 0x00 && Sm4Buffer[i + 2] == 0x01)
				{
					Sm4Buffer[i] = 0x1F;
					Sm4Buffer[i + 1] = 0x2F;
					Sm4Buffer[i + 2] = 0x3F;
					i = i + 3;
				}
				else if(Sm4Buffer[i] == 0x00  && Sm4Buffer[i + 1] == 0x00 && Sm4Buffer[i + 2] == 0x00 && Sm4Buffer[i + 3] == 0x01)
				{
					Sm4Buffer[i] == 0x1F;
					Sm4Buffer[i + 1] = 0x2F;
					Sm4Buffer[i + 2] = 0x3F;
					Sm4Buffer[i + 3] = 0x4F;
					i = i + 4;
				}
			}

			pkt.pts = pts;
			pkt.dts =dts; 

			pkt.stream_index = stream_index;
			pkt.data = Sm4Buffer;


			pkt.flags  = flags;

			fprintf(fp_clzhan, "Encrypt %d  encrypt_output_len = %d\n ", pkt.size, encrypt_output_len);
			for(int i = 0; i < pkt.size; i ++)
			{    
				//printf("%02x ", pkt.data[i]);
				fprintf(fp_clzhan, "%02x ", pkt.data[i]);
			}
			//printf("\n\n");
			fprintf(fp_clzhan,"\n\n");

		}
	}
	else
	{
		pkt.pts = pts;
		pkt.dts = dts;
		pkt.stream_index = stream_index;
		pkt.data = buffer;
		pkt.size = size;
        pkt.flags  = flags;
	}


	ret = av_interleaved_write_frame(m_pFormatCtx, &pkt);
	printf("-----------------clzhan------------------------\n");
    if(ret != 0)
	{
	//	CRYPTO_API int EncryptSM4(const unsigned char *password, const unsigned char *input_data, const size_t input_length, unsigned char *out_data, size_t *output_length);
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Error while clzhan frame ");
        return false;
    }

	return ret;
	
	
}

int OTRecorderFFmpeg::PutAudioDataSM4ToStream(uint8_t * buffer, int size, int stream_index, int flags, int64_t pts, int64_t dts)
{
	printf("write Audio Data size = %d\n", size);
		
	int ret = 0;
	AVPacket pkt = {0};
		
	av_init_packet(&pkt);

	size_t encrypt_output_len = 0;

	memset(Sm4Buffer, 0, Sm4Buffer_len);
	
	// size ? 8
	if(size > 4)
	{

		//unsigned char key[] = "01234567890abcef";

		if (0 != EncryptSM4((unsigned char*)encryptionKey.data(), buffer+4, size - 4, Sm4Buffer + 4, &encrypt_output_len))
		{   
			OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "EncryptSM4 error, buff_password:%s\n", encryptionKey.data());
			return -1;
		}  
		for(int i =0; i < 4;i++)
		{
			Sm4Buffer[i] = buffer[i];
		}

	
		pkt.pts = pts;
		pkt.dts =dts; 

		pkt.stream_index = stream_index;
		pkt.data = Sm4Buffer;
		pkt.size = (int)encrypt_output_len + 4;
		pkt.flags  = flags;


		//fprintf(fp_clzhan, "Encrypt %d\n ", pkt.size);
		//for(int i = 0; i < pkt.size; i ++)
		//{    
			////printf("%02x ", pkt.data[i]);
			//fprintf(fp_clzhan, "%02x ", pkt.data[i]);
		//}
		////printf("\n\n");
		//fprintf(fp_clzhan,"\n\n");


	}
	else
	{
		pkt.pts = pts;
		pkt.dts = dts;
		pkt.stream_index = stream_index;
		pkt.data = buffer;
		pkt.size = size;
        pkt.flags  = flags;

	}

	ret = av_interleaved_write_frame(m_pFormatCtx, &pkt);

    if(ret != 0)
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Error while clzhan frame ");
        return false;
    }

	return ret;
	
	
}

bool OTRecorderFFmpeg::writeRawVideoPayload(const void* yuv420PayPtr, size_t yuv420PaySize)
{
	if(!m_bVideoOpened)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Video stream not opened");
		return false;
	}
	if(((m_pVideoStream->codec->width * m_pVideoStream->codec->height * 3) >> 1) != yuv420PaySize)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "%u not valid buffer size to host yuv420 image(%dx%d)", yuv420PaySize, m_pVideoStream->codec->width, m_pVideoStream->codec->height);
		return false;
	}

	int ret;
	AVPacket pkt = {0};

	// wrap() the data
	/*size_t size =*/ avpicture_fill((AVPicture *)m_pVideoFrame, (const uint8_t*)yuv420PayPtr, PIX_FMT_YUV420P, m_pVideoStream->codec->width, m_pVideoStream->codec->height);
	
	 av_init_packet(&pkt);

	// update presentation time
#if 1
	m_pVideoFrame->pts = av_rescale_q((ot_gettime() - m_nFirstWriteTime), kRecorderBaseTime, m_pVideoStream->time_base);
	pkt.dts = pkt.pts = m_pAudioFrame->pts;
#else
	m_pVideoFrame->pts += av_rescale_q(1, m_pVideoStream->codec->time_base, m_pVideoStream->time_base);
#endif

    if (m_pFormatCtx->oformat->flags & AVFMT_RAWPICTURE)
	{
        /* Raw video case - directly store the picture in the packet */
        pkt.flags        |= AV_PKT_FLAG_KEY;
        pkt.stream_index  = m_pVideoStream->index;
        pkt.data          = m_pVideoFrame->data[0];
        pkt.size          = sizeof(AVPicture);

		m_oMutex->lock();
		//for(int i = 8; i < pkt.size; i ++)
		//{
			//int tmp = pkt.data[i];
			//tmp++;
			//pkt.data[i] = tmp;
		//}
		if(isEncryption)
		{
			ret = PutVideoDataSM4ToStream(pkt.data, pkt.size, pkt.stream_index, pkt.flags, pkt.pts, pkt.dts);
		}
		else
		{
			ret = av_interleaved_write_frame(m_pFormatCtx, &pkt);
		}

		m_oMutex->unlock();
    } 
	else
	{
        /* encode the image */
        int got_output;

        pkt.data = NULL;    // packet data will be allocated by the encoder
        pkt.size = 0;

        ret = avcodec_encode_video2(m_pVideoStream->codec, &pkt, m_pVideoFrame, &got_output);
        if(ret < 0)
		{
            OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Error encoding video frame: %s", ot_av_err2str(ret));
			return false;
        }

        /* If size is zero, it means the image was buffered. */
        if(got_output)
		{
            if(m_pVideoStream->codec->coded_frame->key_frame)
			{
                pkt.flags |= AV_PKT_FLAG_KEY;
			}

            pkt.stream_index = m_pVideoStream->index;

            /* Write the compressed frame to the media file. */
			m_oMutex->lock();
			//for(int i = 8; i < pkt.size; i ++)
			//{
				//int tmp = pkt.data[i];
				//tmp++;
				//pkt.data[i] = tmp;
			//}
			if(isEncryption)
			{
		    	ret = PutVideoDataSM4ToStream(pkt.data, pkt.size, pkt.stream_index, pkt.flags, pkt.pts, pkt.dts);
			}
			else
			{
            	ret = av_interleaved_write_frame(m_pFormatCtx, &pkt);
			}

			m_oMutex->unlock();
        } 
		else
		{
            ret = 0;
        }
    }
    if(ret != 0)
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Error while writing video frame: %s", ot_av_err2str(ret));
        return false;
    }

	return (ret == 0);
}

bool OTRecorderFFmpeg::writeRawAudioPayload(const void* _pcmPayPtr, size_t _pcmPaySize)
{
	if(!m_bAudioOpened)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Audio stream not opened");
		return false;
	}
	const void* pcmPayPtr = _pcmPayPtr;
	size_t pcmPaySize = _pcmPaySize;

	// resample audio buffer
	if(m_oAudioResampler)
	{
		if((pcmPaySize = m_oAudioResampler->resample(_pcmPayPtr, _pcmPaySize)) > 0)
		{
			pcmPayPtr = m_oAudioResampler->getBufferPtr();
		}
	}

	if(!pcmPayPtr || !pcmPaySize)
	{
		return false;
	}

	// Copy data
	size_t nRoom = (m_nAVAudioBufferSizeInBytes - m_nAVAudioBufferIndexInBytes);
	size_t nToCopy = TSK_MIN(nRoom, pcmPaySize);
	if(nToCopy)
	{
		memcpy(&m_pAVAudioBufferPtr[m_nAVAudioBufferIndexInBytes], pcmPayPtr, nToCopy);
		m_nAVAudioBufferIndexInBytes += nToCopy;
	}
	// Do nothing if not entirely filled
	if(m_nAVAudioBufferIndexInBytes < m_nAVAudioBufferSizeInBytes)
	{
		return true;
	}
	
    AVPacket pkt = { 0 }; // data and size must be 0;
    int got_packet, ret;

    av_init_packet(&pkt);
	m_pAudioFrame->nb_samples = m_nAVAudioBufferSizeInSamples;
	m_pAudioFrame->format = m_pAudioStream->codec->sample_fmt;
    ret = avcodec_fill_audio_frame(m_pAudioFrame, 
							m_pAudioStream->codec->channels, 
							m_pAudioStream->codec->sample_fmt,
                            m_pAVAudioBufferPtr,
                            m_nAVAudioBufferSizeInBytes, 
							1);
	// Reset index
	m_nAVAudioBufferIndexInBytes = 0;
	// Copy extra for next time
	if(nToCopy < pcmPaySize)
	{
		size_t nToCopy2 = (pcmPaySize - nToCopy);
		memcpy(&m_pAVAudioBufferPtr[m_nAVAudioBufferIndexInBytes], &((uint8_t*)pcmPayPtr)[nToCopy], nToCopy2);
		m_nAVAudioBufferIndexInBytes += nToCopy2;
	}

	if(ret < 0)
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Error filling audio frame: %s", ot_av_err2str(ret));
        return false;
    }
	
#if 0
	uint64_t nNow = ot_gettime();
	if(!m_nFirstWriteTime)
	{
		m_nFirstWriteTime = nNow;
	}
	else
	{
		m_pAudioFrame->pts += av_rescale_q((nNow - m_nFirstWriteTime), kBaseTimeInMs, m_pAudioStream->time_base);
		pkt.pts = m_pAudioFrame->pts;
		m_nFirstWriteTime = nNow;
	}
#else
	m_pAudioFrame->pts = av_rescale_q((ot_gettime() - m_nFirstWriteTime), kRecorderBaseTime, m_pAudioStream->time_base);
	pkt.dts = pkt.pts = m_pAudioFrame->pts;
#endif

    ret = avcodec_encode_audio2(m_pAudioStream->codec, &pkt, m_pAudioFrame, &got_packet);
    if(ret < 0)
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Error encoding audio frame: %s", ot_av_err2str(ret));
        return false;
    }

    if(!got_packet)
	{
        return true;
	}

    pkt.stream_index = m_pAudioStream->index;
	
    /* Write the compressed frame to the media file. */
	m_oMutex->lock();
	//for(int i = 8; i < pkt.size; i ++)
	//{
		//int tmp = pkt.data[i];
		//tmp++;
		//pkt.data[i] = tmp;
	//}
	//ret = av_interleaved_write_frame(m_pFormatCtx, &pkt);
	ret = PutAudioDataSM4ToStream(pkt.data, pkt.size, pkt.stream_index, pkt.flags, pkt.pts, pkt.dts);
	m_oMutex->unlock();
    if(ret != 0)
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Error while writing audio frame: %s", ot_av_err2str(ret));
        return false;
    }
	
	return true;
}

bool OTRecorderFFmpeg::close(OTMediaType_t eMediaType)
{
	/* Write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
	if(m_pFormatCtx && (m_bAudioOpened || m_bVideoOpened))
	{
		av_write_trailer(m_pFormatCtx);
	}

	if(m_pFormatCtx->pb && !(m_pFormatCtx->oformat->flags & AVFMT_NOFILE) && m_pFormatCtx)
	{
        /* Close the output file. */
        avio_close(m_pFormatCtx->pb);
		m_pFormatCtx->pb = NULL;
	}

	if(m_pVideoFrame)
	{
		av_free(m_pVideoFrame);
		m_pVideoFrame = NULL;
	}

	if(m_pAudioFrame)
	{
		av_free(m_pAudioFrame);
		m_pAudioFrame = NULL;
	}

	_closeAudio();
	_closeVideo();

	/* free the stream */
	if(m_pFormatCtx)
	{
		avformat_free_context(m_pFormatCtx);
		m_pFormatCtx = NULL;
	}
	if(Sm4Buffer)
	{
		av_free(Sm4Buffer);
		Sm4Buffer = NULL;
	}

	fclose(fp_clzhan);
		
	
	//post

	//HttpPost(const std::string & strUrl, const std::string & strMeetingNO, const std::string & Path , std::string & strResponse)

	std::string strResponse;
	HttpPost(m_strRecordOpenFireServlet, m_strConferenceId, m_strFilePath, strResponse);
	if(!strResponse.empty())
	{
		printf(" strResponse: %s\n", strResponse.c_str());
	}


	return true;
}

AVStream* OTRecorderFFmpeg::_addStream(AVCodec **ppCodec, enum AVCodecID eCodecId)
{
	OT_ASSERT(m_pFormatCtx && ppCodec);

	AVCodecContext *pCodecCtx = NULL;
    AVStream *pStream = NULL;

    /* find the encoder */
    *ppCodec = avcodec_find_encoder(eCodecId);
    if(!(*ppCodec))
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Could not find encoder for '%s'", avcodec_get_name(eCodecId));
        return false;
    }

    pStream = avformat_new_stream(m_pFormatCtx, *ppCodec);
    if(!pStream) 
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Could not allocate stream");
        return false;
    }
    pStream->id = m_pFormatCtx->nb_streams - 1;
	pStream->codec->codec_id = eCodecId;

	switch((*ppCodec)->type)
	{
		case AVMEDIA_TYPE_AUDIO:
			{
				// get best sample fmt
				pStream->codec->sample_fmt  = AV_SAMPLE_FMT_S16;
				if((*ppCodec)->sample_fmts)
				{
					const enum AVSampleFormat *p_sample_fmts = (*ppCodec)->sample_fmts;
					for(; *p_sample_fmts != AV_SAMPLE_FMT_NONE; p_sample_fmts++)
					{
						if(*p_sample_fmts == pStream->codec->sample_fmt)
						{
							break;
						}
					}
					if (*p_sample_fmts == AV_SAMPLE_FMT_NONE)
					{
						pStream->codec->sample_fmt = (*ppCodec)->sample_fmts[0];
					}
				}
				OT_DEBUG_INFO_EX(kOTMobuleNameFFmpegRecorder, "Added audio stream with id=%d and codec=%s to the container for recording", pStream->id, avcodec_get_name(eCodecId));
				break;
			}

		case AVMEDIA_TYPE_VIDEO:
			{
				OT_DEBUG_INFO_EX(kOTMobuleNameFFmpegRecorder, "Added video stream with id=%d and codec=%s to the container for recording", pStream->id, avcodec_get_name(eCodecId));
				pStream->codec->pix_fmt = AV_PIX_FMT_YUV420P;
				if(pStream->codec->codec_id == AV_CODEC_ID_MPEG1VIDEO)
				{
					/* Needed to avoid using macroblocks in which some coeffs overflow.
					 * This does not happen with normal video, it just happens here as
					 * the motion of the chroma plane does not match the luma plane. */
					pStream->codec->mb_decision = 2;
				}
				break;
			}
	}

	/* Some formats want stream headers to be separate. */
    if (m_pFormatCtx->oformat->flags & AVFMT_GLOBALHEADER)
	{
		pStream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}

	return pStream;
}

bool OTRecorderFFmpeg::_openAudio()
{
	if(m_bAudioOpened || m_pAudioFrame || m_pAVAudioBufferPtr || m_oAudioResampler)
	{
		OT_DEBUG_WARN_EX(kOTMobuleNameFFmpegRecorder, "Audio stream already opened (or partially)");
		return true;
	}

	if(!m_pAudioStream)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "No audio stream in container");
		return false;
	}
	
    int ret;

	/* allocate and init a re-usable frame */
    m_pAudioFrame = avcodec_alloc_frame();
    if(!m_pAudioFrame)
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Could not allocate audio frame");
        return false;
    }
	m_pAudioFrame->pts = 0;
	
    /* open it */
	AVDictionary *opts = NULL;
#if 0 // required for FFmpeg native AAC but we prefer libfaac
	ret = av_dict_set(&opts, "strict", "experimental", 0);
#endif
    ret = avcodec_open2(m_pAudioStream->codec, m_pAudioCodec, &opts);
    if (ret < 0)
	{
		av_dict_free(&opts);
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Could not open audio codec: %s", ot_av_err2str(ret));
        return false;
    }
	av_dict_free(&opts);

	/* guess required frame size */
	if(m_pAudioStream->codec->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)
	{
        m_nAVAudioBufferSizeInSamples = 1024;
	}
    else
	{
        m_nAVAudioBufferSizeInSamples = m_pAudioStream->codec->frame_size;
	}
	m_nAVAudioBufferSizeInBytes = (
				m_nAVAudioBufferSizeInSamples * 
				av_get_bytes_per_sample(m_pAudioStream->codec->sample_fmt) * 
				m_pAudioStream->codec->channels);
	if(!(m_pAVAudioBufferPtr = (uint8_t*)av_malloc(m_nAVAudioBufferSizeInBytes)))
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Failed to allocate buffer with size = %u", m_nAVAudioBufferSizeInBytes);
		m_nAVAudioBufferSizeInBytes = m_nAVAudioBufferSizeInSamples = 0;
		return false;
	}
	OT_DEBUG_INFO_EX(kOTMobuleNameFFmpegRecorder, "Recording AudioBufferSizeInSamples: %u", m_nAVAudioBufferSizeInSamples);

	if(m_pAudioStream->codec->sample_fmt != AV_SAMPLE_FMT_S16)
	{
		m_oAudioResampler = new OTAudioResamplerFFmpeg(
				m_pAudioStream->codec,
				AV_SAMPLE_FMT_S16, /* Assume BitsPerSample equal to 16 */
				m_pAudioStream->codec->channels,
				m_pAudioStream->codec->sample_rate
			);

		if(!m_oAudioResampler || !m_oAudioResampler->isValid())
		{
			OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Failed to create audio resampler");
			return false;
		}
	}

	m_bAudioOpened = true;

	return true;
}

bool OTRecorderFFmpeg::_closeAudio()
{
	if(m_pAudioStream)
	{
		avcodec_close(m_pAudioStream->codec);
		// m_pAudioStream->codec = NULL; /* avformat_free_context() crash if set to NULL and streams not successfully added */
	}
	if(m_pAVAudioBufferPtr)
	{
		av_free(m_pAVAudioBufferPtr);
		m_pAVAudioBufferPtr = NULL;
	}

	m_oAudioResampler = NULL;

	m_bAudioOpened = false;
	
	return true;
}


bool OTRecorderFFmpeg::_openVideo()
{
	if(m_bVideoOpened || m_pVideoFrame)
	{
		OT_DEBUG_WARN_EX(kOTMobuleNameFFmpegRecorder, "Video stream already opened (or partially)");
		return true;
	}

	if(!m_pVideoStream)
	{
		OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "No audio stream in container");
		return false;
	}

	int ret;
	
    /* open the codec */
    ret = avcodec_open2(m_pVideoStream->codec, m_pVideoCodec, NULL);
    if(ret < 0)
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Could not open video codec: %s", ot_av_err2str(ret));
        return false;
    }

    /* allocate and init a re-usable frame */
    m_pVideoFrame = avcodec_alloc_frame();
    if(!m_pVideoFrame)
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Could not allocate video frame");
        return false;
    }
	m_pVideoFrame->pts = 0;

    /* Allocate the encoded raw picture. */
    ret = avpicture_alloc((AVPicture*)m_pVideoFrame, m_pVideoStream->codec->pix_fmt, m_pVideoStream->codec->width, m_pVideoStream->codec->height);
    if(ret < 0)
	{
        OT_DEBUG_ERROR_EX(kOTMobuleNameFFmpegRecorder, "Could not allocate picture: %s", ot_av_err2str(ret));
        return false;
    }

    m_bVideoOpened = true;

	return true;
}

bool OTRecorderFFmpeg::_closeVideo()
{
	if(m_pVideoStream)
	{
		avcodec_close(m_pVideoStream->codec);
		// m_pVideoStream->codec = NULL; /* avformat_free_context() crash if set to NULL and streams not successfully added */
	}
	
	if(m_pVideoFrame)
	{
		av_free(m_pVideoFrame);
		m_pVideoFrame = NULL;
	}

	m_bVideoOpened = false;

	return true;
}


//
//	OTRecorderWebM
//



extern void Ebml_Init(struct EbmlGlobal** ppGlob, FILE* pFile);
extern void Ebml_DeInit(struct EbmlGlobal** ppGlob);
extern void write_webm_file_header(struct EbmlGlobal *glob,
                       const struct EbmlRational *fps,
					   unsigned int pixelWidth,
					   unsigned int pixelHeight,
                       stereo_format_t stereo_fmt);
extern void write_webm_block(struct EbmlGlobal *glob,
				 bool isInvisible,
				 bool isKeyFrame,
				 unsigned pts,
				 EbmlRational fps,
                 const void* dataPtr, size_t dataSize);
extern void write_webm_file_footer(EbmlGlobal *glob, long hash);

OTRecorderWebM::OTRecorderWebM(bool isEncryption,std::string encryptionKey, std::string Sm2PublicKey, 
											std::string strRecordOpenFireServlet, 
											std::string strRecordFile,
											std::string strConferenceId,
											std::string strFilePath, OTMediaType_t eMediaType)
: OTRecorder(isEncryption, encryptionKey, Sm2PublicKey, strRecordOpenFireServlet,strRecordFile,strConferenceId,strFilePath, eMediaType)
, m_pFile(NULL)
, m_pGlob(NULL)
, m_nVideoWidth(0)
, m_nVideoHeight(0)
, m_nPts(0)
{
	m_fps.num = 1;
	m_fps.den = (int)15;

	m_Buffer.Ptr = NULL;
	m_Buffer.Size = m_Buffer.index = 0;
}

OTRecorderWebM::~OTRecorderWebM()
{
	close(m_eMediaType); // write footer and close file
	if(m_Buffer.Ptr)
	{
		free(m_Buffer.Ptr);
	}
	OT_DEBUG_INFO("*** OTRecorder destroyed ***");
}

bool OTRecorderWebM::setVideoParams(uint32_t nWidth, uint32_t nVideoHeight, uint32_t nFps)
{
	m_fps.num = 1;
	m_fps.den = (int)nFps;

	m_nVideoWidth = nWidth;
	m_nVideoHeight = nVideoHeight;

	return true;
}

bool OTRecorderWebM::open(OTMediaType_t eMediaType)
{
	if(!m_pFile)
	{
		m_pFile = fopen(m_strFilePath.c_str(), "wb");
		OT_ASSERT(m_pFile);
		
		Ebml_Init(&m_pGlob, m_pFile);
		OT_ASSERT(m_pGlob);
	}
	
	if((eMediaType & OTMediaType_Video) == OTMediaType_Video)
	{
		// write WebM file header
		OT_ASSERT(m_nVideoWidth && m_nVideoHeight);
		write_webm_file_header(m_pGlob, &m_fps,
							   m_nVideoWidth, m_nVideoHeight,
							   STEREO_FORMAT_MONO);
	}

	return true;
}

bool OTRecorderWebM::writeRtpVideoPayload(const void* rtpPayPtr, size_t rtpPaySize)
{
	OT_ASSERT(m_pFile && m_pGlob);
	const uint8_t* pcData = (const uint8_t*)rtpPayPtr;
	const uint8_t* pcDataEnd = (pcData + rtpPaySize);
	uint8_t S, PartID;

	{	/* draft-ietf-payload-vp8-08 - 4.2.  VP8 Payload Descriptor */
		uint8_t X, R, N, I, L, T, K;//FIXME: store
		
		X = (*pcData & 0x80)>>7;
		R = (*pcData & 0x40)>>6;
		if(R)
		{
			OT_DEBUG_ERROR_EX(kOTMobuleNameWebM, "R<>0");
			return false;
		}
		N = (*pcData & 0x20)>>5;
		S = (*pcData & 0x10)>>4;
		PartID = (*pcData & 0x0F);
		// skip "REQUIRED" header
		if(++pcData >= pcDataEnd)
		{ 
			OT_DEBUG_ERROR_EX(kOTMobuleNameWebM, "Too short");
			return false;
		}
		// check "OPTIONAL" headers
		if(X)
		{
			I = (*pcData & 0x80);
			L = (*pcData & 0x40);
			T = (*pcData & 0x20);
			K = (*pcData & 0x10);
			if(++pcData >= pcDataEnd)
			{ 
				OT_DEBUG_ERROR_EX(kOTMobuleNameWebM, "Too short");
				return false; 
			}

			if(I)
			{
				if(*pcData & 0x80) // M
				{
					// PictureID on 16bits 
					if((pcData += 2) >= pcDataEnd)
					{ 
						OT_DEBUG_ERROR_EX(kOTMobuleNameWebM, "Too short");
						return false;
					}
				}
				else
				{
					// PictureID on 8bits
					if(++pcData >= pcDataEnd)
					{ 
						OT_DEBUG_ERROR_EX(kOTMobuleNameWebM, "Too short"); 
						return false; 
					}
				}
			}
			if(L)
			{
				if(++pcData >= pcDataEnd)
				{ 
					OT_DEBUG_ERROR_EX(kOTMobuleNameWebM, "Too short");
					return false;
				}
			}
			if(T || K)
			{
				if(++pcData >= pcDataEnd)
				{ 
					OT_DEBUG_ERROR_EX(kOTMobuleNameWebM, "Too short");
					return false;
				}
			}
		}
	}

	if(S && PartID == 0) // First partition?
	{
		if(m_Buffer.index > 0) // pending data?
		{
			/* 4.3.  VP8 Payload Header */
			bool bInvisible = (m_Buffer.Ptr[0] & 0x10) ? false : true; /* H */
			bool bKeyFrame = (m_Buffer.Ptr[0] & 0x01) ? false : true; /* P */
			write_webm_block(m_pGlob, bInvisible, bKeyFrame, ++m_nPts, m_fps, m_Buffer.Ptr, m_Buffer.index);
			m_Buffer.index = 0;
		}
	}

	size_t nDataSize = (pcDataEnd - pcData);
	if((m_Buffer.index + nDataSize) > m_Buffer.Size)
	{
		m_Buffer.Ptr = (uint8_t*)realloc(m_Buffer.Ptr, (m_Buffer.index + nDataSize));
		if(!m_Buffer.Ptr)
		{
			OT_DEBUG_ERROR_EX(kOTMobuleNameWebM, "Failed to allocate buffer with size = %u", (m_Buffer.index + nDataSize));
			m_Buffer.index = m_Buffer.Size = 0;
			return false;
		}
		m_Buffer.Size = (m_Buffer.index + nDataSize);
	}
	memcpy(&m_Buffer.Ptr[m_Buffer.index], pcData, nDataSize);
	m_Buffer.index += nDataSize;
	
	return true;
}

bool OTRecorderWebM::writeRtpAudioPayload(const void* rtpPayPtr, size_t rtpPaySize)
{
	OT_ASSERT(m_pFile);
	return true;
}

bool OTRecorderWebM::close(OTMediaType_t eMediaType/*FIXME: not used yet*/)
{
	if(m_pGlob && m_pFile)
	{
		static const long hash = 0;
		// write WebM file footer (requires file to be open)
		write_webm_file_footer(m_pGlob, hash);
		
		// destroy context (without closing the file)
		Ebml_DeInit(&m_pGlob);
	}

	if(m_pFile)
	{
		fclose(m_pFile);
		m_pFile = NULL;
	}
	return true;
}
