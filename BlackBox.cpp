#include "StdAfx.h"

#include <time.h>
#include "FFMPEGLibrary.h"
#include "BlackBox.h"

#define MAX_AUDIO_PACKET_SIZE (1024 * 1024 * 4)

CBlackBox::CBlackBox(void)
{
	Initialize();
}

CBlackBox::CBlackBox(bool isIPCamera)
{
	bIsIPCamera = isIPCamera;
	Initialize();
}

void CBlackBox::Initialize()
{
	videoMainCircularBufferQueue = gcnew ConcurrentQueue<CVideoAudioMetaData^>();
	videobackgroundCircularBufferQueue = gcnew ConcurrentQueue<CVideoAudioMetaData^>();
	
	dtStartBuffering = gcnew time_t();
	*dtStartBuffering = 0;

	eclipeTotalSeconds = 0;
	iBrightness = 0;
	iBrightnessSky = 0;	
	iCircularBufferLength = 30;
	iVideoFileChumkLength = 20;
	iFrameRate = 30;

	isRecordingStarted = false;
	isStartNewChuckVideoRecording = false;
	enable24HrsRecording = true;

	sizeofFrame = Size(1280, 720);
	recordFileDateTimeFormat = "ddMMyyyyHHmmss";
}

#pragma region CicularBufferQueue Control
void CBlackBox::AddVideoAudioFrame(CVideoAudioMetaData ^ tCVideoAudioMetaData)
{
	try
	{
		if (enable24HrsRecording)
		{
			OpenFFmpeg::AVPacket *backgroundVideoPacket = (OpenFFmpeg::AVPacket*)malloc(sizeof(OpenFFmpeg::AVPacket));
			OpenFFmpeg::av_init_packet(backgroundVideoPacket);
			OpenFFmpeg::av_packet_ref(backgroundVideoPacket, tCVideoAudioMetaData->videoPacket);

			CVideoAudioMetaData ^ newBackgroundMetaDataPacket = gcnew CVideoAudioMetaData(backgroundVideoPacket, nullptr, 0, 0);
			videobackgroundCircularBufferQueue->Enqueue(newBackgroundMetaDataPacket);
		}

		if (commitAll)
		{
			//Let the working thread save all video to file
			return;
		}

		if (*dtStartBuffering == 0)
		{
			*dtStartBuffering = tCVideoAudioMetaData->metaDataTimeStamp;
			
			FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, "Main Pre-event Buffer Queue Started");
		}

		videoMainCircularBufferQueue->Enqueue(tCVideoAudioMetaData);
		eclipeTotalSeconds = (int) (tCVideoAudioMetaData->metaDataTimeStamp - *dtStartBuffering);

		if (eclipeTotalSeconds > iCircularBufferLength)
		{
			CVideoAudioMetaData ^ tCVideoAudioMetaDataOld;
			for (; ;)
			{
				videoMainCircularBufferQueue->TryPeek(tCVideoAudioMetaDataOld);
				eclipeTotalSeconds = (int) (tCVideoAudioMetaData->metaDataTimeStamp - *dtStartBuffering);
				if (eclipeTotalSeconds > iCircularBufferLength)
				{
					videoMainCircularBufferQueue->TryDequeue(tCVideoAudioMetaDataOld);
					*dtStartBuffering = tCVideoAudioMetaDataOld->metaDataTimeStamp;

					//delete the this one
					OpenFFmpeg::av_packet_unref(tCVideoAudioMetaDataOld->videoPacket);
					OpenFFmpeg::av_free(tCVideoAudioMetaDataOld->videoPacket);

					delete tCVideoAudioMetaDataOld;
				}
				else
				{
					break;
				}
			}
		}
	}
	catch (...)
	{
	}
}
#pragma endregion

#pragma region Background recording & Control
void CBlackBox::Start24HrsVideoArchive()
{
	enable24HrsRecording = true;
	bStopBackgroundArchive = false;

	Thread^ backgroundVideoWriteThread = gcnew Thread(gcnew ThreadStart(this, &CBlackBox::backgroundVideoWriterThreadProc));
	backgroundVideoWriteThread->Name = "Background Video Writer Thread Proc";
	backgroundVideoWriteThread->Priority = ThreadPriority::BelowNormal;
	backgroundVideoWriteThread->Start();

	return;
}

void CBlackBox::Stop24HrsVideoArchive()
{
	enable24HrsRecording = false;
	bStopBackgroundArchive = true;

	////Wait for thread recording exits for 2 seconds
	int iIndex = 0;
	while (bStopBackgroundArchive)
	{
		iIndex++;
		Thread::Sleep(500);

		if (iIndex >= 4)
		{
			break;
		}
	}

	FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, "Background video recording stopped");

	return;
}

void my_log_callback(void *ptr, int level, const char *fmt, va_list vargs)
{
	//printf(fm);// t, vargs);
	FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, String::Format(" {0}, {1}", "Write background file error.: ", *fmt));
}

void CBlackBox::backgroundVideoWriterThreadProc()
{
	//Do all the background video (24 hr) video writing here
	//Video should be written as chunk of specific length of iVideoFileChumkLength
	//Wait for pCodecCtx initialize
	while (FFmpegVideoLibrary::FFmpegVideo::pCodecCtx == NULL)
	{
		if (bStopBackgroundArchive)
		{
			bStopBackgroundArchive = false;
			return;
		}

		Thread::Sleep(10);
	}
	OpenFFmpeg::AVFormatContext	*oFmtCtx_b = NULL;

	time_t dtRecordStart;
	time_t dtRecordCurrent;
	time_t dtRecordTime = 0;

	FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, "Background video recording started");

	//Enable FFMPEG log
	//OpenFFmpeg::av_log_set_level(AV_LOG_DEBUG);
	//OpenFFmpeg::av_log_set_callback(my_log_callback);

	CVideoAudioMetaData ^ newBackgroundMetaDataPacket;
	while (!bStopBackgroundArchive)
	{
		try
		{
			time(&dtRecordStart);
			tm *ltm = localtime(&dtRecordStart);
			
			// George Sun
			// Add the support of Logitech camera
			//bIsLogitech = true;
			//String ^videoFileExtension = bIsLogitech ? ".avi" : ".mp4";
			//String ^videoFileExtension = bIsIPCamera ? ".mp4" : ".avi";
			String ^videoFileExtension = ".mp4";
			String ^ArchvideoFile = String::Format("Rec_{0:00}{1:00}{2}{3:00}{4:00}{5:00}{6}", ltm->tm_mday, ltm->tm_mon + 1, 1900 + ltm->tm_year, ltm->tm_hour, ltm->tm_min, ltm->tm_sec, videoFileExtension);
			if (DefaultVideoFileRootFolder->Length > 0)
			{
				ArchvideoFile = System::IO::Path::Combine(DefaultVideoFileRootFolder, ArchvideoFile);
			}

			OpenFFmpeg::AVOutputFormat* ofmt = OpenFFmpeg::av_guess_format(NULL, FFmpegVideoLibrary::FFmpegVideo::ManagedStringToUnmanagedUTF8Char(ArchvideoFile), NULL);
			OpenFFmpeg::avformat_alloc_output_context2(&oFmtCtx_b, ofmt, NULL, NULL);

			OpenFFmpeg::AVStream* oVideoStream = OpenFFmpeg::avformat_new_stream(oFmtCtx_b, NULL);
			if (OpenFFmpeg::avcodec_copy_context(oVideoStream->codec, FFmpegVideoLibrary::FFmpegVideo::pCodecCtx) < 0)
			{
				return;
			}
			oVideoStream->codec->codec_tag = 0;

			oFmtCtx_b->oformat = ofmt;
			if (!(oFmtCtx_b->oformat->flags & AVFMT_NOFILE))
			{
				if (OpenFFmpeg::avio_open(&oFmtCtx_b->pb, FFmpegVideoLibrary::FFmpegVideo::ManagedStringToUnmanagedUTF8Char(ArchvideoFile), AVIO_FLAG_WRITE) < 0)
				{
					//printf("can not open output file handle!\n");  
					return;
				}
			}

			//George Sun
			// Add MJPEG option
			//oFmtCtx_b->streams[0]->codec->codec_id = bIsLogitech ? OpenFFmpeg::AV_CODEC_ID_MJPEG : OpenFFmpeg::AV_CODEC_ID_H264;
			oFmtCtx_b->streams[0]->codec->codec_id = OpenFFmpeg::AV_CODEC_ID_H264;

			int iRet = OpenFFmpeg::avformat_write_header(oFmtCtx_b, NULL);
			if (iRet < 0)
			{
				//printf("can not write the header of the output file!\n");
				return;
			}

			oFmtCtx_b->streams[0]->time_base.num = 1;
			if (bIsIPCamera)
			{
				//oFmtCtx_b->streams[0]->time_base.den *= 2;  //for GPIO, 10.0.9.208
				oFmtCtx_b->streams[0]->time_base.den *= oFmtCtx_b->streams[0]->codec->ticks_per_frame;  //for GPIO, 10.0.9.208
			}
			else
			{
				if (!bIsLogitech)
				{
					oFmtCtx_b->streams[0]->time_base.num = 950;
					oFmtCtx_b->streams[0]->time_base.den = 4000 * FrameRate;  //for ELP USB camera
				}
			}
			int den0 = FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->time_base.den;
			int num0 = FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->time_base.num;
			int den1 = oFmtCtx_b->streams[0]->time_base.den;
			int num1 = oFmtCtx_b->streams[0]->time_base.num;

			dtRecordTime = 0;
			int file_W_error = 0;
			int framecount = 0;
			while (dtRecordTime <= 1200)	//Alway set background video chuck to 20 minutes, 20*60 Seconds
			{
				if (bStopBackgroundArchive)
				{
					break;
				}

				try
				{
					if (videobackgroundCircularBufferQueue->Count > 0)
					{
						if (videobackgroundCircularBufferQueue->TryDequeue(newBackgroundMetaDataPacket))
						{
							OpenFFmpeg::AVPacket *packet = newBackgroundMetaDataPacket->videoPacket;

							packet->pts = av_rescale_q(packet->pts, FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->time_base, oFmtCtx_b->streams[0]->time_base);
							packet->dts = av_rescale_q(packet->dts, FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->time_base, oFmtCtx_b->streams[0]->time_base);
							//packet->pts = framecount++;	for Logitech camera MJPEG direct saving only
							packet->dts = packet->pts;

							packet->stream_index = oVideoStream->id;

							if (OpenFFmpeg::av_interleaved_write_frame(oFmtCtx_b, packet) != 0)
							{
								file_W_error++;
								FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, String::Format(" {0}, {1}", "Write background file error.: ", file_W_error));
							}
							else
							{
								Thread::Sleep(28);
							}

							OpenFFmpeg::av_free_packet(packet);
							delete newBackgroundMetaDataPacket;
						}
					}

					time(&dtRecordCurrent);
					dtRecordTime = dtRecordCurrent - dtRecordStart;
				}
				catch (...)
				{
				}
			}

			if (OpenFFmpeg::av_write_trailer(oFmtCtx_b) != 0)
			{
				//FireEvent("Close recording file error.");
				OpenFFmpeg::av_free(ofmt);
			}

			if (oFmtCtx_b != NULL)
			{
				if (oFmtCtx_b->pb != NULL)
				{
					OpenFFmpeg::avio_close(oFmtCtx_b->pb);
				}

				if (oFmtCtx_b->oformat != NULL)
				{
					OpenFFmpeg::av_free(oFmtCtx_b->oformat);
				}

				OpenFFmpeg::avformat_free_context(oFmtCtx_b);
			}

			//Fire the event to infor save to file complete
			FireVideoSavedEvent(ArchvideoFile);
		}
		catch (...)
		{

		}

	}

	bStopBackgroundArchive = false;
}

#pragma endregion

#pragma region Main event recording & control
bool CBlackBox::StartRecording(String ^videoFileName, int frameRate, int bitRate)
{
	isRecordingStarted = true;
	bStopRecording = false;
	commitAll = false;

	ThreadParams^ param = gcnew ThreadParams(videoFileName, frameRate, bitRate);

	Thread^ recordingThread = gcnew Thread(gcnew ParameterizedThreadStart(this, &CBlackBox::MainVideoWriterThreadProc));
	recordingThread->Name = "Video Recording Thread Proc";
	recordingThread->Priority = ThreadPriority::Highest;
	recordingThread->Start(param);

	return true;
}

bool CBlackBox::StartNewChuckVideoRecording(String ^newVideoFileName)
{
	*dtStartBuffering = time(0);

	isStartNewChuckVideoRecording = true;
	newChuckVideoFileName = newVideoFileName;
	return true;
}

void CBlackBox::MainVideoWriterThreadProc(Object^ paramObj)
{
	ThreadParams^ tParam = (ThreadParams^)paramObj;
	String^ videofileName = tParam->videoFileName;

	WriterPrivateData ^data = gcnew WriterPrivateData();
	try
	{
		while (FFmpegVideoLibrary::FFmpegVideo::pCodecCtx == NULL)
		{
			Thread::Sleep(10);
			//printf("pCodecCtx = %d\n", pCodecCtx);
		}

NewVideoFile:
		OpenFFmpeg::AVFormatContext	*oFmtCtx = NULL;
		OpenFFmpeg::AVOutputFormat* ofmt = OpenFFmpeg::av_guess_format(NULL, FFmpegVideoLibrary::FFmpegVideo::ManagedStringToUnmanagedUTF8Char(videofileName), NULL);
		OpenFFmpeg::avformat_alloc_output_context2(&oFmtCtx, ofmt, NULL, NULL);

		OpenFFmpeg::AVStream* oVideoStream = OpenFFmpeg::avformat_new_stream(oFmtCtx, NULL);
		if (OpenFFmpeg::avcodec_copy_context(oVideoStream->codec, FFmpegVideoLibrary::FFmpegVideo::pCodecCtx) < 0)
		{
			return;
		}
		oVideoStream->codec->codec_tag = 0;

		oVideoStream->codec->codec = FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->codec;
		data->FormatContext = oFmtCtx;
		if (oFmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
		{
			oVideoStream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
		}

		oFmtCtx->oformat = ofmt;
		oVideoStream->codec->codec_tag = 0;
		m_frame_num = 0;
		m_audio_frame = 0;
		m_no_sound = false;
		m_frameRate = 30;
		data->SampleRate = 44100;
		data->BitRate = 1411200;
		//int audioCodec = OpenFFmpeg::CODEC_ID_AAC;
		data->Channels = 2;
		add_audio_stream(data, (OpenFFmpeg::AVCodecID) OpenFFmpeg::CODEC_ID_AAC);
		open_audio(data);

		if (!(oFmtCtx->oformat->flags & AVFMT_NOFILE))
		{
			if (OpenFFmpeg::avio_open(&oFmtCtx->pb, FFmpegVideoLibrary::FFmpegVideo::ManagedStringToUnmanagedUTF8Char(videofileName), AVIO_FLAG_WRITE) < 0)
			{
				//printf("can not open output file handle!\n");  
				return;
			}
		}

		oFmtCtx->streams[0]->codec->codec_id = OpenFFmpeg::AV_CODEC_ID_H264;
		m_totalAudioSamples = 0;
		m_cur_pts_a = 0;
		m_cur_pts_v = 0;
		m_total_delay = 0;

		if (OpenFFmpeg::avformat_write_header(oFmtCtx, NULL) < 0)
		{
			goto ClosingVideoFile;
		}

		oFmtCtx->streams[0]->time_base.num = 1;
		if (bIsIPCamera)
		{
			oFmtCtx->streams[0]->time_base.den *= oFmtCtx->streams[0]->codec->ticks_per_frame;  //for GPIO, 10.0.9.208
		}
		else
		{
//			oFmtCtx->streams[0]->time_base.den = 5 * FrameRate;  //for ELP USB camera
			oFmtCtx->streams[0]->time_base.num = 986;
			oFmtCtx->streams[0]->time_base.den = 4000 * m_frameRate;

		}

		int file_W_error = 0;
		CVideoAudioMetaData ^ tCVideoAudioMetaData;
		while (!bStopRecording)
		{
			try
			{
				if (commitAll)// && videoCircularBufferQueue.Count <= 0)
				{
					Thread::Sleep(35); //wait for last frame, Write remain frames
					//System.Diagnostics.Trace.WriteLine(string.Format("Remaining frames {0}", videoCircularBufferQueue.Count));

					while (!videoMainCircularBufferQueue->IsEmpty)
					{
						if (videoMainCircularBufferQueue->TryDequeue(tCVideoAudioMetaData))
						{
							OpenFFmpeg::AVPacket *packet = tCVideoAudioMetaData->videoPacket;

							packet->pts = av_rescale_q(packet->pts, FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->time_base, oFmtCtx->streams[0]->time_base);
							packet->dts = av_rescale_q(packet->dts, FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->time_base, oFmtCtx->streams[0]->time_base);
							packet->stream_index = oVideoStream->id;

							if (OpenFFmpeg::av_interleaved_write_frame(oFmtCtx, packet) != 0)
							{
								file_W_error++;
								FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, String::Format(" {0}, {1}", "Write background file error.: ", file_W_error));
								//break;
							}

							if (tCVideoAudioMetaData->audioBufferBytes != nullptr)
							{
								array<Byte>^pByte2 = tCVideoAudioMetaData->audioBufferBytes;
								{
									WriteAudio(data, pByte2, tCVideoAudioMetaData->audioBufferLen);
								}
							}

							OpenFFmpeg::av_free_packet(packet);
							delete tCVideoAudioMetaData;
						}
					}


					break;
				}

				//if ((currentFrameDT - chuckStartingDT).TotalSeconds >= defaultVideoChuck)
				if (isStartNewChuckVideoRecording && newChuckVideoFileName->Length > 0)
				{
					//Refresh FFMPEG writer with new file name for the chuck
					goto ClosingVideoFile;
				}

				//Write 35 frames to file in one block
				for (int i = 0; i < 35; i++)
				{
					if (videoMainCircularBufferQueue->Count > 0)
					{
						if (videoMainCircularBufferQueue->TryDequeue(tCVideoAudioMetaData))
						{
							OpenFFmpeg::AVPacket *packet = tCVideoAudioMetaData->videoPacket;

							System::Diagnostics::Trace::WriteLine("Audio Date InQueue = " + tCVideoAudioMetaData->audioBufferLen.ToString());

							packet->pts = av_rescale_q(packet->pts, FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->time_base, oFmtCtx->streams[0]->time_base);
							packet->dts = av_rescale_q(packet->dts, FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->time_base, oFmtCtx->streams[0]->time_base);
							packet->stream_index = oVideoStream->id;

							if (OpenFFmpeg::av_interleaved_write_frame(oFmtCtx, packet) != 0)
							{
								file_W_error++;
								FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, String::Format(" {0}, {1}", "Write background file error.: ", file_W_error));
								//break;
							}

							if (tCVideoAudioMetaData->audioBufferBytes != nullptr)
							{
								array<Byte>^pByte2 = tCVideoAudioMetaData->audioBufferBytes;
								{
									WriteAudio(data, pByte2, tCVideoAudioMetaData->audioBufferLen);
								}
							}

							OpenFFmpeg::av_free_packet(packet);
							delete tCVideoAudioMetaData;
						}
						Thread::Sleep(15);
					}
					else
					{
						Thread::Sleep(30); //Wait for at least one frame
						break;
					}
				}
			}
			catch (...)
			{
			}
		}
	ClosingVideoFile:

		if (OpenFFmpeg::av_write_trailer(oFmtCtx) != 0)
		{
			OpenFFmpeg::av_free(ofmt);
		}

		if (oFmtCtx != NULL)
		{
			if (oFmtCtx->pb != NULL)
			{
				OpenFFmpeg::avio_close(oFmtCtx->pb);
			}

			if (oFmtCtx->oformat != NULL)
			{
				OpenFFmpeg::av_free(oFmtCtx->oformat);
			}

			OpenFFmpeg::avformat_free_context(oFmtCtx);
		}

		if (isStartNewChuckVideoRecording && newChuckVideoFileName->Length > 0)
		{
			isStartNewChuckVideoRecording = false;

			//Fire the event to inform save to file complete
			FireVideoSavedEvent(videofileName);

			videofileName = newChuckVideoFileName;
			//*dtStartBuffering = 0;

			goto NewVideoFile;
		}
	}
	catch (...)
	{
		//System::Diagnostics::Trace::WriteLine("CommitToFile Error, " + ex.what());
		//FFmpegVideoLibrary::FFmpegVideo::FireEvent("Commit To File, Error...");
	}
	finally
	{
		//Fire the event to inform save to file complete
		FireVideoSavedEvent(videofileName);

		Reset();

		FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, "Saved Video File: " + videofileName);
	}
}

void CBlackBox::ResetArchiveCircularBuffer()
{
	while (!videobackgroundCircularBufferQueue->IsEmpty)
	{
		CVideoAudioMetaData ^ tCVideoAudioMetaData;
		videobackgroundCircularBufferQueue->TryDequeue(tCVideoAudioMetaData);

		delete tCVideoAudioMetaData;
	}
}

void CBlackBox::add_audio_stream(WriterPrivateData^ data, enum OpenFFmpeg::AVCodecID codec_id)
{
	OpenFFmpeg::AVCodecContext *codecContex;

	data->AudioStream = OpenFFmpeg::avformat_new_stream(data->FormatContext, 0);

	if (!data->AudioStream)
	{
		throw gcnew System::IO::IOException("Failed creating new audio stream.");
	}

	// Codec.
	codecContex = data->AudioStream->codec;
	codecContex->codec_id = codec_id;
	codecContex->codec_type = OpenFFmpeg::AVMEDIA_TYPE_AUDIO;
	// Set format
	codecContex->bit_rate = data->BitRate;
	codecContex->sample_rate = data->SampleRate;
	codecContex->channels = data->Channels;

	codecContex->sample_fmt = OpenFFmpeg::AV_SAMPLE_FMT_S16;

	codecContex->time_base.num = 1;
	codecContex->time_base.den = codecContex->sample_rate;


	data->AudioEncodeBufferSize = 4 * MAX_AUDIO_PACKET_SIZE;
	if (data->AudioEncodeBuffer == NULL)
	{
		data->AudioEncodeBuffer = (uint8_t*)OpenFFmpeg::av_malloc(data->AudioEncodeBufferSize);
	}

	// Some formats want stream headers to be separate.
	if (data->FormatContext->oformat->flags & AVFMT_GLOBALHEADER)
	{
		codecContex->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}
}

void CBlackBox::open_audio(WriterPrivateData^ data)
{
	OpenFFmpeg::AVCodecContext* codecContext_a = data->AudioStream->codec;
	OpenFFmpeg::AVCodec* codec = avcodec_find_encoder(codecContext_a->codec_id);
	if (!codec)
	{
		throw gcnew System::IO::IOException("Cannot find audio codec.");
	}

	// Open it.
	//if (libffmpeg::avcodec_open(codecContext, codec) < 0) 
	if (avcodec_open2(codecContext_a, codec, NULL) < 0)
	{
		//printf("Cannot open audio codec\n");
		return;
	}

	if (codecContext_a->frame_size <= 1)
	{
		// Ugly hack for PCM codecs (will be removed ASAP with new PCM
		// support to compute the input frame size in samples. 
		data->AudioInputSampleSize = data->AudioEncodeBufferSize / codecContext_a->channels;
		switch (codecContext_a->codec_id)
		{
		case OpenFFmpeg::CODEC_ID_PCM_S16LE:
		case OpenFFmpeg::CODEC_ID_PCM_S16BE:
		case OpenFFmpeg::CODEC_ID_PCM_U16LE:
		case OpenFFmpeg::CODEC_ID_PCM_U16BE:
			data->AudioInputSampleSize >>= 1;
			break;
		default:
			break;
		}
		codecContext_a->frame_size = data->AudioInputSampleSize;
	}
	else
	{
		data->AudioInputSampleSize = codecContext_a->frame_size;
	}
}

void CBlackBox::WriteAudio(WriterPrivateData^ data, array<Byte>^ soundBuffer, int soundBufferSize)
{
	//CheckIfDisposed();

	if (data == nullptr)
	{
		throw gcnew System::IO::IOException("A video file was not opened yet.");
	}

	// Add sound
	AddAudioSamples(data, soundBuffer, soundBufferSize);
}
void CBlackBox::AddAudioSamples(WriterPrivateData^ data, array<Byte>^  soundBuffer, int soundBufferSize)
{
	int a_step = 1024;
	if (!data->AudioStream)
		return;
	//System::Diagnostics::Trace::WriteLine("Audio SOUND BUFFER Size ################= " + soundBufferSize);
	if (soundBufferSize == 0)
	{
		m_no_sound = true;
		m_cur_pts_a += 512;//a_step;
		System::Diagnostics::Trace::WriteLine("No sound, pts= " + m_cur_pts_a);
	}
	try
	{
		OpenFFmpeg::AVCodecContext* codecContext = data->AudioStream->codec;

		//memcpy(data->AudioBuffer + data->AudioBufferSizeCurrent, (uint8_t *)soundBuffer, soundBufferSize);
		//Array::Copy(soundBuffer, 0, data->AudioBuffer), data->AudioBufferSizeCurrent, soundBufferSize);


		pin_ptr<System::Byte> p = &soundBuffer[0];// &tCVideoAudioMetaData->audioBufferBytes[0];


												  //m_cur_pts_a = m_cur_pts_v;
		unsigned char* pby = p;
		char* pch = reinterpret_cast<char*>(pby);

		if (pch > 0)
		{
			memcpy(data->AudioBuffer + data->AudioBufferSizeCurrent, pch, soundBufferSize);

			//System::Diagnostics::Trace::WriteLine("Audio Date copied");
		}

		data->AudioBufferSizeCurrent += soundBufferSize;

		BYTE* pSoundBuffer = (BYTE *)data->AudioBuffer;
		DWORD nCurrentSize = data->AudioBufferSizeCurrent;

		// Size of packet on bytes.
		// FORMAT s16
		DWORD packSizeInSize = (2 * data->AudioInputSampleSize) * data->Channels;

		if (nCurrentSize >= packSizeInSize)
		{
			while (nCurrentSize >= packSizeInSize)
			{
				OpenFFmpeg::AVPacket packet;
				OpenFFmpeg::av_init_packet(&packet);

				packet.size = OpenFFmpeg::avcodec_encode_audio(codecContext, data->AudioEncodeBuffer, data->AudioEncodeBufferSize, (const short *)pSoundBuffer);
				m_audio_frame++;
				m_totalAudioSamples += packSizeInSize;
				packet.pts = m_cur_pts_a;//AV_NOPTS_VALUE;//
				packet.dts = packet.pts;
				m_cur_pts_a += a_step;
				packet.flags |= AV_PKT_FLAG_KEY;
				packet.stream_index = data->AudioStream->index;
				packet.data = data->AudioEncodeBuffer;

				// Write the compressed frame in the media file.
				if (OpenFFmpeg::av_interleaved_write_frame(data->FormatContext, &packet) != 0)
				{
					break;
				}

				OpenFFmpeg::av_free_packet(&packet);
				nCurrentSize -= packSizeInSize;
				pSoundBuffer += packSizeInSize;

				uint64_t new_pts_a = static_cast<int64_t>(m_totalAudioSamples * codecContext->time_base.den / 4 / data->SampleRate);
				//System::Diagnostics::Trace::WriteLine("a_pts ################= " +  m_cur_pts_a);
				//if (new_pts_a <= m_cur_pts_a)
				//{
				//	m_cur_pts_a += 23;//1000 / m_frameRate;
				//	//Console::WriteLine(" -Moment {0}: Encounter a moment with no audio reading.", m_totalSeconds);
				//}
				//else
				//m_cur_pts_a = new_pts_a *1008/1000;
			}
		}
		else
		{
			//Calculate to get 2 seconds.  Rear camera starts recording blocks out (delay audio) for 2 seconds (1.69)
			//Console::WriteLine("PTS->{0}", data->VideoFrame->pts);
			//Write first 2 seconds 0 byte audio data to video frame
			//double delaySeconds = (float) data-> / m_frameRate;
			//Console::WriteLine("PTS->{0}", data->FrameNumber);
			OpenFFmpeg::AVPacket packet;
			OpenFFmpeg::av_init_packet(&packet);

			packet.size = OpenFFmpeg::avcodec_encode_audio(codecContext, data->AudioEncodeBuffer, data->AudioEncodeBufferSize, (const short *)pSoundBuffer);
			//if (delaySeconds <= audioFrames)//100)
			{
				packet.pts = AV_NOPTS_VALUE;
				packet.dts = AV_NOPTS_VALUE;
			}
			/*else
			{
			packet.pts = cur_pts_v;
			packet.dts = packet.pts;
			}*/

			packet.flags |= AV_PKT_FLAG_KEY;
			packet.stream_index = data->AudioStream->index;
			packet.data = data->AudioEncodeBuffer;

			// Write the compressed frame in the media file.
			OpenFFmpeg::av_interleaved_write_frame(data->FormatContext, &packet);

			OpenFFmpeg::av_free_packet(&packet);
		}

		// save excess
		memcpy(data->AudioBuffer, data->AudioBuffer + data->AudioBufferSizeCurrent - nCurrentSize, nCurrentSize);
		data->AudioBufferSizeCurrent = nCurrentSize;
	}
	catch (...)
	{
	}
}

bool CBlackBox::CommitAll()
{
	commitAll = true;
	//System.Diagnostics.Trace.WriteLine(string.Format("CommitAll {0}", commitAll.ToString()));

	//clean up some old frame and start
	//ResetArchiveCircularBuffer();
	return true;
}

void CBlackBox::Reset()
{
	isRecordingStarted = false;
	*dtStartBuffering = 0;
	commitAll = false;
}

void CBlackBox::FireVideoSavedEvent(String^ videoFileName)
{
	FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::VideoSaved, videoFileName);
}

#pragma endregion


