/*
* Copyright (C) 2013 Mamadou DIOP
* Copyright (C) 2013 Doubango Telecom <http://www.doubango.org>
* License: GPLv3
* This file is part of the open source SIP TelePresence system <https://code.google.com/p/telepresence/>
*/
#ifndef OPENTELEPRESENCE_RECODER_H
#define OPENTELEPRESENCE_RECODER_H

#include "OpenTelepresenceConfig.h"
#include "opentelepresence/OTObject.h"
#include "opentelepresence/OTCommon.h"
#include "opentelepresence/recorder/OTWebM.h"

#include <string>

class OTRecorder : public OTObject
{
protected:
	OTRecorder(bool isEncryption,std::string encryptionKey, std::string Sm2PublicKey,
										std::string strRecordOpenFireServlet, 
										std::string strRecordFile,
										std::string strConferenceId,
										std::string strFilePath, OTMediaType_t eMediaType);
public:
	virtual ~OTRecorder();
	virtual OT_INLINE const char* getObjectId() { return "OTRecorder"; }

	virtual bool setVideoParams(uint32_t nVideoWidth, uint32_t nVideoHeight, uint32_t nMotionRank, uint32_t nGopSizeInSec, uint32_t nFps) = 0;
	virtual bool setAudioParams(uint32_t nPtime, uint32_t nRate, uint32_t nChannels) = 0;
	virtual bool open(OTMediaType_t eMediaType) = 0;
	virtual bool writeRtpVideoPayload(const void* rtpPayPtr, size_t rtpPaySize) = 0;
	virtual bool writeRtpAudioPayload(const void* rtpPayPtr, size_t rtpPaySize) = 0;
	virtual bool writeRawVideoPayload(const void* rtpPayPtr, size_t rtpPaySize) = 0;
	virtual bool writeRawAudioPayload(const void* rtpPayPtr, size_t rtpPaySize) = 0;
	virtual bool close(OTMediaType_t eMediaType) = 0;

	static OTObjectWrapper<OTRecorder*> New(bool isEncryption,std::string encryptionKey, std::string Sm2PublicKey,
																						 std::string strRecordOpenFireServlet, 
																						 std::string strRecordFile,
																						 std::string strConferenceId,
																						 std::string strFilePath, 
																						 OTMediaType_t eMediaType);

protected:
	bool isEncryption;
	std::string encryptionKey; //AES or SM4
	std::string Sm2PublicKey;
	std::string m_strRecordOpenFireServlet;
	std::string m_strRecordFile;   //120100_20160112 filename
	std::string m_strConferenceId; //120100
	std::string m_strFilePath;
	OTMediaType_t m_eMediaType;
};

#endif /* OPENTELEPRESENCE_RECODER_H */
