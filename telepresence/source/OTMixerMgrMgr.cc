/*
* Copyright (C) 2013 Mamadou DIOP
* Copyright (C) 2013 Doubango Telecom <http://www.doubango.org>
* License: GPLv3
* This file is part of the open source SIP TelePresence system <https://code.google.com/p/telepresence/>
*/
#include "opentelepresence/OTMixerMgrMgr.h"
#include "opentelepresence/OTMixerMgrAudio.h"
#include "opentelepresence/OTMixerMgrVideo.h"

#include <assert.h>
#include <time.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>


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


OTMixerMgrMgr::OTMixerMgrMgr(OTMediaType_t eMediaType, OTObjectWrapper<OTBridgeInfo*> oBridgeInfo)
:OTObject()
{
	m_pOTMixerMgrAudio = NULL;
	m_bValid = false;
	m_eMediaType = eMediaType;

	// create recorder if enabled
	if(oBridgeInfo->isRecordEnabled())
	{


		// get 120100's sm4key and sm2key or use default key
		



		//mkdir /mnt/2016/1/9/120100/

		time_t tt = time(NULL);
		tm* t= localtime(&tt);

		std::string year = int2str(t->tm_year + 1900);
		std::string month = int2str(t->tm_mon + 1);
		std::string day = int2str(t->tm_mday);

		std::string filePath(oBridgeInfo->getRecordFilePath() + oBridgeInfo->getFromId() + "/" + year + "/" + month + "/" + day + "/");

		create_multi_dir(filePath.data());

		// set /mnt/2016/1/9/120100/id.mp4

		std::string strRecordFile(filePath + oBridgeInfo->getId() + "." + oBridgeInfo->getRecordFileExt());

		printf("strRecordFile = %s \n", strRecordFile.data());

		//new  recoder

		m_oRecorder = OTRecorder::New(strRecordFile, eMediaType);

		printf("from id = %s\n", oBridgeInfo->getFromId().data());
		printf("id = %s\n", oBridgeInfo->getId().data());
		//std::string strRecordFile(oBridgeInfo->getRecordFilePath() + oBridgeInfo->getId() + "." + oBridgeInfo->getRecordFileExt());
	}

	if(m_eMediaType & OTMediaType_Audio)
	{
		OTObjectWrapper<OTMixerMgr*> pOTMixerMgr = OTMixerMgr::New(OTMediaType_Audio, oBridgeInfo);
		m_pOTMixerMgrAudio = dynamic_cast<OTMixerMgrAudio*>(*pOTMixerMgr);
		if(!m_pOTMixerMgrAudio)
		{
			OT_DEBUG_ERROR("Failed to create audio mixer...die()");
			OT_ASSERT(false);
		}
		if(m_oRecorder)
		{
			m_oRecorder->setAudioParams(oBridgeInfo->getAudioPtime(), oBridgeInfo->getAudioSampleRate(), oBridgeInfo->getAudioChannels());
			m_pOTMixerMgrAudio->setRecorder(m_oRecorder); // set recorder
		}
	}
	if(m_eMediaType & OTMediaType_Video)
	{
		OTObjectWrapper<OTMixerMgr*> pOTMixerMgr = OTMixerMgr::New(OTMediaType_Video, oBridgeInfo);
		m_pOTMixerMgrVideo = dynamic_cast<OTMixerMgrVideo*>(*pOTMixerMgr);
		OT_ASSERT(*m_pOTMixerMgrVideo);
		if(m_oRecorder)
		{
			m_oRecorder->setVideoParams(oBridgeInfo->getVideoWidth(), oBridgeInfo->getVideoHeight(), oBridgeInfo->getVideoMotionRank(), oBridgeInfo->getVideoGopSizeInSec(), oBridgeInfo->getVideoFps());
			m_pOTMixerMgrVideo->setRecorder(m_oRecorder); // set recorder
		}
	}

	m_bValid = (m_pOTMixerMgrAudio || m_pOTMixerMgrVideo);

	if(m_bValid)
	{
		if(m_oRecorder)
		{
			m_oRecorder->open(m_eMediaType);
		}
	}
}

OTMixerMgrMgr::~OTMixerMgrMgr()
{
	OT_DEBUG_INFO("*** OTMixerMgrMgr destroyed ***");
}

bool OTMixerMgrMgr::isValid()
{
	return m_bValid;
}

int OTMixerMgrMgr::start(OTMediaType_t eMediaType)
{
	int ret = 0;

	if((eMediaType & OTMediaType_Audio) && m_pOTMixerMgrAudio)
	{
		if((ret = m_pOTMixerMgrAudio->start()))
		{
			return ret;
		}
	}
	if((eMediaType & OTMediaType_Video) && m_pOTMixerMgrVideo)
	{
		if((ret = m_pOTMixerMgrVideo->start()))
		{
			return ret;
		}
	}
	return 0;
}

bool OTMixerMgrMgr::isStarted(OTMediaType_t eMediaType)
{
	return false;
}

int OTMixerMgrMgr::pause(OTMediaType_t eMediaType)
{
	int ret = 0;

	if((eMediaType & OTMediaType_Audio) && m_pOTMixerMgrAudio)
	{
		if((ret = m_pOTMixerMgrAudio->pause()))
		{
			return ret;
		}
	}
	if((eMediaType & OTMediaType_Video) && m_pOTMixerMgrVideo)
	{
		if((ret = m_pOTMixerMgrVideo->pause()))
		{
			return ret;
		}
	}
	return 0;
}

bool OTMixerMgrMgr::isPaused(OTMediaType_t eMediaType)
{
	return false;
}

int OTMixerMgrMgr::flush(OTMediaType_t eMediaType)
{
	int ret = 0;

	if((eMediaType & OTMediaType_Audio) && m_pOTMixerMgrAudio)
	{
		if((ret = m_pOTMixerMgrAudio->flush()))
		{
			return ret;
		}
	}
	if((eMediaType & OTMediaType_Video) && m_pOTMixerMgrVideo)
	{
		if((ret = m_pOTMixerMgrVideo->flush()))
		{
			return ret;
		}
	}
	return 0;
}

int OTMixerMgrMgr::stop(OTMediaType_t eMediaType)
{
	int ret = 0;

	if((eMediaType & OTMediaType_Audio) && m_pOTMixerMgrAudio)
	{
		if((ret = m_pOTMixerMgrAudio->stop()))
		{
			return ret;
		}
	}
	if((eMediaType & OTMediaType_Video) && m_pOTMixerMgrVideo)
	{
		if((ret = m_pOTMixerMgrVideo->stop()))
		{
			return ret;
		}
	}
	return 0;
}

int OTMixerMgrMgr::attachMediaPlugins(OTObjectWrapper<OTSessionInfo*> pOTSessionInfo)
{
	int ret = 0;
	if(m_pOTMixerMgrAudio && pOTSessionInfo->haveAudio())
	{
		OT_ASSERT(dynamic_cast<OTSessionInfoAV*>(*pOTSessionInfo));
		if((ret = m_pOTMixerMgrAudio->attachMediaPlugins(pOTSessionInfo)))
		{
			return ret;
		}
	}
	if(m_pOTMixerMgrVideo && pOTSessionInfo->haveVideo())
	{
		OT_ASSERT(dynamic_cast<OTSessionInfoAV*>(*pOTSessionInfo));
		if((ret = m_pOTMixerMgrVideo->attachMediaPlugins(pOTSessionInfo)))
		{
			return ret;
		}
	}

	return ret;
}

int OTMixerMgrMgr::deAttachMediaPlugins(OTObjectWrapper<OTSessionInfo*> pOTSessionInfo)
{
	int ret = 0;
	if(m_pOTMixerMgrAudio && pOTSessionInfo->haveAudio())
	{
		OT_ASSERT(dynamic_cast<OTSessionInfoAV*>(*pOTSessionInfo));
		if((ret = m_pOTMixerMgrAudio->deAttachMediaPlugins(pOTSessionInfo)))
		{
			return ret;
		}
	}
	if(m_pOTMixerMgrVideo && pOTSessionInfo->haveVideo())
	{
		OT_ASSERT(dynamic_cast<OTSessionInfoAV*>(*pOTSessionInfo));
		if((ret = m_pOTMixerMgrVideo->deAttachMediaPlugins(pOTSessionInfo)))
		{
			return ret;
		}
	}

	return 0;
}

OTObjectWrapper<OTMixerMgrMgr*> OTMixerMgrMgr::New(OTMediaType_t eMediaType, OTObjectWrapper<OTBridgeInfo*> oBridgeInfo)
{
	OTObjectWrapper<OTMixerMgrMgr*> pOTMixerMgr = new OTMixerMgrMgr(eMediaType, oBridgeInfo);
	if(pOTMixerMgr && !pOTMixerMgr->isValid())
	{
		OTObjectSafeRelease(pOTMixerMgr);
	}
	return pOTMixerMgr;
}
