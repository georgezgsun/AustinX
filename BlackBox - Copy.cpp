#include "StdAfx.h"

#include <time.h>
#include "FFMPEGLibrary.h"
#include "BlackBox.h"

CBlackBox::CBlackBox(void)
{
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

void CBlackBox::backgroundVideoWriterThreadProc()
{
	//Do all the background video (24 hr) video writing here
	//Video should be written as chunk of specific length of iVideoFileChumkLength
	//OpenFFmpeg::AVCodecContext	*pCodecCtx1 = FFmpegVideoLibrary::FFmpegVideo::pCodecCtx;
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

	CVideoAudioMetaData ^ newBackgroundMetaDataPacket;
	while (!bStopBackgroundArchive)
	{
		try
		{
			time(&dtRecordStart);
			tm *ltm = localtime(&dtRecordStart);
			
			// George Sun
			// Add the support of Logitech camera
			bIsLogitech = true;
			String ^videoFileExtension = bIsLogitech ? ".avi" : ".mp4";
			String ^ArchvideoFile = String::Format("Rec_{0:00}{1:00}{2}{3:00}{4:00}{5:00}{6}", ltm->tm_mday, ltm->tm_mon + 1, 1900 + ltm->tm_year, ltm->tm_hour, ltm->tm_min, ltm->tm_sec, videoFileExtension);
			if (DefaultVideoFileRootFolder->Length > 0)
			{
				ArchvideoFile = System::IO::Path::Combine(DefaultVideoFileRootFolder, ArchvideoFile);
			}

			OpenFFmpeg::AVOutputFormat* ofmt = OpenFFmpeg::av_guess_format(NULL, FFmpegVideoLibrary::FFmpegVideo::ManagedStringToUnmanagedUTF8Char(ArchvideoFile), NULL);
			OpenFFmpeg::avformat_alloc_output_context2(&oFmtCtx_b, ofmt, NULL, NULL);
			OpenFFmpeg::AVStream* oVideoStream = OpenFFmpeg::avformat_new_stream(oFmtCtx_b, NULL);
			OpenFFmpeg::avcodec_copy_context(oVideoStream->codec, FFmpegVideoLibrary::FFmpegVideo::pCodecCtx);
			oVideoStream->codec->codec = FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->codec;

			if (oFmtCtx_b->oformat->flags & AVFMT_GLOBALHEADER)
			{
				oVideoStream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
			}

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

			if (OpenFFmpeg::avformat_write_header(oFmtCtx_b, NULL) < 0)
			{
				//printf("can not write the header of the output file!\n");
				return;
			}

			//if (cameraBrand == "GPIO")
			oFmtCtx_b->streams[0]->time_base.den *= 2;  //for GPIO, 10.0.9.208
														//else if (cameraBrand == "InHouse")
														//	oFmtCtx_b->streams[0]->time_base.den = 10; 
			dtRecordTime = 0;
			int file_W_error = 0;
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

							packet->stream_index = oVideoStream->id;

							if (OpenFFmpeg::av_write_frame(oFmtCtx_b, packet) != 0)
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

		OpenFFmpeg::avcodec_copy_context(oVideoStream->codec, FFmpegVideoLibrary::FFmpegVideo::pCodecCtx);
		oVideoStream->codec->codec = FFmpegVideoLibrary::FFmpegVideo::pCodecCtx->codec;

		if (oFmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
		{
			oVideoStream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
		}

		oFmtCtx->oformat = ofmt;

		if (!(oFmtCtx->oformat->flags & AVFMT_NOFILE))
		{
			if (OpenFFmpeg::avio_open(&oFmtCtx->pb, FFmpegVideoLibrary::FFmpegVideo::ManagedStringToUnmanagedUTF8Char(videofileName), AVIO_FLAG_WRITE) < 0)
			{
				//printf("can not open output file handle!\n");  
				return;
			}
		}

		oFmtCtx->streams[0]->codec->codec_id = OpenFFmpeg::AV_CODEC_ID_H264;

		if (OpenFFmpeg::avformat_write_header(oFmtCtx, NULL) < 0)
		{
			goto ClosingVideoFile;
		}

		oFmtCtx->streams[0]->time_base.den *= 2;  //for GPIO, 10.0.9.208
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

							if (OpenFFmpeg::av_write_frame(oFmtCtx, packet) != 0)
							{
								file_W_error++;
								FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, String::Format(" {0}, {1}", "Write background file error.: ", file_W_error));
								//break;
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

							if (OpenFFmpeg::av_write_frame(oFmtCtx, packet) != 0)
							{
								file_W_error++;
								FFmpegVideoLibrary::FFmpegVideo::FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, String::Format(" {0}, {1}", "Write background file error.: ", file_W_error));
								//break;
							}
							OpenFFmpeg::av_free_packet(packet);
							delete tCVideoAudioMetaData;
						}
					}
					else
					{
						Thread::Sleep(34); //Wait for at least one frame
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


