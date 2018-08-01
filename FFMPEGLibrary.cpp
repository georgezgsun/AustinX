// This is the main DLL file.
#include "stdafx.h"
#include <stdio.h>
#include <vcclr.h>
#include <exception>
#include <cstdint>
#include <time.h>
#include <string.h>

#include "FFMPEGLibrary.h"
#include "VideoAudioMetaData.h"


namespace FFmpegVideoLibrary
{
	BITMAPINFO bmpInfo;
	RECT rc;

	static HBITMAP ghBM = NULL;
	BITMAPV5HEADER bi;

	#pragma region Library Initialization & Control
	static int InterruptCallback(void *ctx)
	{
		OpenFFmpeg::AVFormatContext* formatContext = reinterpret_cast<OpenFFmpeg::AVFormatContext*>(ctx);

		//timeout after 5 seconds of no activity
		int timeOutMilliseconds = GetTickCount();
		if (formatContext->start_time < 0)
		{
			formatContext->start_time = timeOutMilliseconds;
		}
		else if (formatContext->start_time > 0 && ((timeOutMilliseconds - formatContext->start_time) > 5000))
		{
			return 1;
		}

		return 0;
	}

	void FFmpegVideo::InitialLize()
	{
		//Callback function for detecting IP camera off-line or not existing.
		static const OpenFFmpeg::AVIOInterruptCB int_cb = { InterruptCallback, NULL };

		//Control variables
		bStopRequested = false;
		bThreadExited = false;

		//Video variables
		iSkyBrightness = 0;
		takePhotoRequest = false;

		uint8_t *pixelsPtr_ = NULL;
		globalAudioDataBufferLock = gcnew Object();
		audioBytes = gcnew array<Byte>(MAX_AUDIO_PACKET_SIZE);
		isAudioBuffer = false;
	}

	void FFmpegVideo::Connect()
	{
		if (circularBlackBox != nullptr)
		{
			delete circularBlackBox;
		}
		circularBlackBox = gcnew CBlackBox(IsIPCamera);

		if (cameraConnectionPath->Length <= 0)
		{
			return;
		}

		circularBlackBox->DefaultVideoFileRootFolder = VideoRecordRootFilePath;
		circularBlackBox->VideoPreEventLength = VideoPreeventLength;
		Thread^ videoFrameThread = gcnew Thread(gcnew ThreadStart(this, &FFmpegVideo::videoFrameThreadProc));
		videoFrameThread->Name = "videoFrameThread";

		videoFrameThread->Start();

		//Start audio monitoring
		InitialLizeAudioMonitor(true);

		isConnected = true;
		FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, "Library Started");
	}

	void FFmpegVideo::Disconnect()
	{
		FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, "Library Stopping");

		bStopRequested = true;
		Thread::Sleep(50);

		//while (bThreadExited)
		//{
		//	Thread::Sleep(100);
		//}
		bThreadExited = false;
		isConnected = false;

		if (circularBlackBox != nullptr)
		{
			delete circularBlackBox;
		}

	}

	void FFmpegVideo::StartMainEventRecord(String^ videoFileName)
	{
		if (circularBlackBox != nullptr)
		{
			isAudioBuffer = true;

			circularBlackBox->DefaultVideoChuck = this->DefaultVideoChuck;
			circularBlackBox->StartRecording(videoFileName, circularBlackBox->FrameRate, circularBlackBox->VideoFrameSize.Width*circularBlackBox->VideoFrameSize.Height* 10);

		}
	}

	void FFmpegVideo::StopMainEventRecord()
	{
		if (circularBlackBox != nullptr)
		{
			circularBlackBox->CommitAll();
		}
	}

	void FFmpegVideo::StartNewChuckVideoRecording(String^ videoFileName)
	{
		if (circularBlackBox != nullptr)
		{
			circularBlackBox->StartNewChuckVideoRecording(videoFileName);
		}
	}

	void FFmpegVideo::StartBackgroundRecord()
	{
		if (circularBlackBox != nullptr)
		{
			circularBlackBox->Start24HrsVideoArchive();
		}
	}

	void FFmpegVideo::StopBackgroundRecord()
	{
		if (circularBlackBox != nullptr)
		{
			circularBlackBox->Stop24HrsVideoArchive();
		}
	}

	void FFmpegVideo::TakePhotoImageAction()
	{
		this->takePhotoRequest = true;
	}

	#pragma endregion

	#pragma region Sound Data Buffering Functions
	void FFmpegVideo::InitialLizeAudioMonitor(bool startMonitoring)
	{
		try
		{
			if (!startMonitoring)
			{
				if (frontAudioWaveIn)
				{
					frontAudioWaveIn->DataAvailable -= gcnew System::EventHandler<NAudio::Wave::WaveInEventArgs ^>(this, &FFmpegVideoLibrary::FFmpegVideo::OnDataAvailable);
					delete frontAudioWaveIn;
				}
				return;
			}

			//int audioDeviceID = Properties.Settings.Default.SelectedAudioDevice;  //First audio
			int audioDeviceID = GetCodecWaveInAudioID(audioInputDeviceName, 0);
			if (audioDeviceID < 0)
			{
				return;
			}

			frontAudioWaveIn = gcnew WaveIn();
			frontAudioWaveIn->BufferMilliseconds = 30;
			frontAudioWaveIn->DeviceNumber = audioDeviceID;
			frontAudioWaveIn->DataAvailable += gcnew System::EventHandler<NAudio::Wave::WaveInEventArgs ^>(this, &FFmpegVideoLibrary::FFmpegVideo::OnDataAvailable);

			frontAudioWaveIn->WaveFormat = gcnew NAudio::Wave::WaveFormat(44100, 16, NAudio::Wave::WaveIn::GetCapabilities(audioDeviceID).Channels);
			frontAudioWaveIn->StartRecording();

			FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, audioInputDeviceName + " - audio device connected");
		}
		catch (...)
		{
		}
		finally
		{
		}
	}

	int FFmpegVideo::GetCodecWaveInAudioID(String ^ audioDeviceName, int iStartIndex)
	{
		if (audioDeviceName->Length <= 0)
		{
			return -1;
		}

		//Try 5 times
		for (int i = 0; i < 5; i++)
		{
			int waveInDevices = NAudio::Wave::WaveIn::DeviceCount;
			for (int waveInDevice = 0; waveInDevice < waveInDevices; waveInDevice++)
			{
				NAudio::Wave::WaveInCapabilities deviceInfo = NAudio::Wave::WaveIn::GetCapabilities(waveInDevice);
				if (audioDeviceName == deviceInfo.ProductName)
				{
					//break out if found
					return waveInDevice;
				}
			}
		}

		return -1;
	}

	void FFmpegVideoLibrary::FFmpegVideo::OnDataAvailable(System::Object ^sender, NAudio::Wave::WaveInEventArgs ^e)
	{
		//if (!isAudioBuffer)
		//{
		//	return;
		//}

		try
		{
			Monitor::Enter(globalAudioDataBufferLock);

			if (audioByteIndex + e->BytesRecorded > MAX_AUDIO_PACKET_SIZE)
			{
				System::Diagnostics::Trace::WriteLine("Audio Buffer Overflow, Size = " + (audioByteIndex + e->BytesRecorded).ToString());
				Array::Copy(e->Buffer, 0, audioBytes, 0, e->Buffer->Length);
				audioByteIndex = e->Buffer->Length;
			}
			else
			{
				Array::Copy(e->Buffer, 0, audioBytes, audioByteIndex, e->Buffer->Length);
				audioByteIndex += e->Buffer->Length;
	
				//System::Diagnostics::Trace::WriteLine("Incoming Audio Buffer Size = " + e->BytesRecorded.ToString());
			}
		}
		catch (...)
		{

		}
		finally
		{
			Monitor::Exit(globalAudioDataBufferLock);
		}
	}

	#pragma endregion

	#pragma region Library Thread functions
	void FFmpegVideo::videoFrameThreadProc()
	{
		OpenFFmpeg::AVFormatContext	*pFormatCtx = NULL;
		OpenFFmpeg::AVCodec			*pCodec;

		OpenFFmpeg::AVFrame	*pFrame = NULL;
		OpenFFmpeg::AVPicture pic;
		OpenFFmpeg::SwsContext *photoCtxt = NULL;
		OpenFFmpeg::AVInputFormat *ifmt = NULL;

		int iCount = 0;
		bThreadExited = false;

		try
		{
			OpenFFmpeg::av_register_all();
			OpenFFmpeg::avcodec_register_all();	// George Sun
			OpenFFmpeg::avformat_network_init();

			pFormatCtx = OpenFFmpeg::avformat_alloc_context();
			//Add callback Interrupt function
			pFormatCtx->interrupt_callback.callback = InterruptCallback;
			pFormatCtx->interrupt_callback.opaque = pFormatCtx;

			OpenFFmpeg::avdevice_register_all();
			OpenFFmpeg::AVDictionary* dictionary = NULL;

			if (bIsIPCamera)
			{
				//Register callback for read frame in case camera dead for IP CAMERA ONLY
				pFormatCtx->flags |= AVFMT_FLAG_NONBLOCK;
				bIsLogitech = false;
			}
			else
			{
				//Set camera properties
				ifmt = OpenFFmpeg::av_find_input_format("dshow");

				bIsLogitech = cameraConnectionPath->Contains("Logitech");
				if (bIsLogitech)
				{
					OpenFFmpeg::av_dict_set(&dictionary, "video_pin_name", "0", NULL);	//Logitech C920 do not support H264
					OpenFFmpeg::av_dict_set(&dictionary, "vcodec", "mjpeg", NULL);	//Logitech C920 select vcodec=mjpeg
				}
				else
				{
					OpenFFmpeg::av_dict_set(&dictionary, "video_pin_name", "1", NULL);	//ELP camera pin 1 for H264
				}

				cameraConnectionPath = "video=" + cameraConnectionPath;
			}

			OpenFFmpeg::av_dict_set(&dictionary, "video_size", "1280x720", NULL);
			//OpenFFmpeg::av_dict_set(&dictionary, "framerate", "20", NULL);

			//IP camera
			if(OpenFFmpeg::avformat_open_input(&pFormatCtx, ManagedStringToUnmanagedUTF8Char(cameraConnectionPath), ifmt, &dictionary)!=0)
			{
				FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::CameraCriticalError, String::Format("Error : {0}, {1}", "Couldn't open input stream", cameraConnectionPath));
				return;
			}


			if(OpenFFmpeg::avformat_find_stream_info(pFormatCtx,NULL)<0)
			{
				FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::CameraCriticalError, "Couldn't find video stream information");
				return;
			}

			int videoindex = -1;
			for(int i=0; i< (int) pFormatCtx->nb_streams; i++) 
			{
				if(pFormatCtx->streams[i]->codec->codec_type == OpenFFmpeg::AVMEDIA_TYPE_VIDEO)
				{
					videoindex = i;
					break;
				}
			}
			if(videoindex == -1)
			{
				FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::CameraCriticalError, "Couldn't find a video stream");
				return;
			}

			pCodecCtx = pFormatCtx->streams[videoindex]->codec;
			OpenFFmpeg::AVCodecContext* pLocalCodecCtx = pCodecCtx;
			pCodec = OpenFFmpeg::avcodec_find_decoder(pCodecCtx->codec_id);
			if(pCodec == NULL)
			{
				FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::CameraCriticalError, "Video CODEC not found");
				return;
			}

			//Open stream video
			if(OpenFFmpeg::avcodec_open2(pCodecCtx, pCodec,NULL)<0)
			{
				FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::CameraCriticalError, "Could not open video CODEC");
				return;
			}

			//Set the width and height
			::SecureZeroMemory(&bmpInfo, sizeof(bmpInfo));
			bmpInfo.bmiHeader.biBitCount = 24;
			bmpInfo.bmiHeader.biHeight = pCodecCtx->height;
			bmpInfo.bmiHeader.biWidth = pCodecCtx->width;
			bmpInfo.bmiHeader.biPlanes = 1;
			bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmpInfo.bmiHeader.biCompression = BI_RGB;

			//George Sun
			// Setup the codec parameters
			OpenFFmpeg::AVCodec *codec = OpenFFmpeg::avcodec_find_encoder(OpenFFmpeg::CODEC_ID_H264);
			OpenFFmpeg::AVCodecContext* cctx = NULL;
			cctx = OpenFFmpeg::avcodec_alloc_context3(codec);
			OpenFFmpeg::AVPacket *pkt;

			cctx->bit_rate = 4000000;
			cctx->width = pCodecCtx->width;
			cctx->height = pCodecCtx->height;
			cctx->time_base.den = 1;
			cctx->time_base.num = 30;
			//cctx->time_base = pCodecCtx->time_base;	// copy the time base;
			cctx->gop_size = 15;
			cctx->max_b_frames = 1;
			cctx->pix_fmt = OpenFFmpeg::PIX_FMT_YUV422P;	//need to be verified 
			cctx->level = 31;

			cctx->refs = 4;

			// - options: cabac=1 ref=1 deblock=1:0:0 analyse=0x3:0x113 me=hex subme=2 psy=1 psy_rd=1.00:0.00 mixed_ref=0 me_range=16 chroma_me=1 
			//				trellis=0 8x8dct=1 cqm=0 deadzone=21,11 fast_pskip=1 chroma_qp_offset=0 threads=6 lookahead_threads=2 sliced_threads=0 
			//				nr=0 decimate=1 interlaced=0 bluray_compat=0 constrained_intra=0 bframes=3 b_pyramid=2 b_adapt=1 b_bias=0 direct=1 weightb=1 
			//				open_gop=0 weightp=1 keyint=250 keyint_min=25 scenecut=40 intra_refresh=0 rc_lookahead=10 rc=crf mbtree=1 crf=23.0 qcomp=0.60 qpmin=0 qpmax=69 qpstep=4 ip_ratio=1.40 aq=1:1.00
			//cctx->qmax = 51;
			//cctx->qmin = 10;
			//cctx->max_qdiff = 4;
			//cctx->i_quant_factor = 0.71;
			//cctx->coder_type = 1;
			//cctx->has_b_frames = 0;
			//cctx->max_b_frames=3;
			//cctx->qcompress = 0.6;
			//cctx->qblur = 0.5;
			//cctx->me_subpel_quality = 1;
			//cctx->me_range = 16;
			//cctx->me_cmp = 1;
			//cctx->profile = FF_PROFILE_H264_MAIN;
			//cctx->level = 31;
			//cctx->scenechange_threshold = 40;
			//cctx->me_subpel_quality = 5;
			//cctx->bit_rate_tolerance = 40000;
			//cctx->keyint_min = 50;
			//cctx->me_subpel_quality = 0;
			//cctx->noise_reduction = 100;
			//cctx->b_frame_strategy = 1;
			//cctx->me_method = 7;
			//cctx->me_subpel_quality = 5;
			//cctx->flags |= CODEC_FLAG_LOOP_FILTER;
			//cctx->flags2 |= CODEC_FLAG2_BPYRAMID;
			//cctx->flags2 |= CODEC_FLAG2_WPRED;
			//cctx->flags2 |= CODEC_FLAG2_8X8DCT;
			//cctx->rc_max_rate = 0;
			//cctx->rc_buffer_size = 0;
			//cctx->mb_threshold = 1;
			//cctx->compression_level = 10;


			//cctx->profile = FF_PROFILE_H264_BASELINE; 
			OpenFFmpeg::AVDictionary * codec_options(0);
			OpenFFmpeg::av_dict_set(&codec_options, "preset", "faster", 0);
			OpenFFmpeg::av_dict_set(&codec_options, "crf", "23", 0);
			//OpenFFmpeg::av_opt_set(cctx->priv_data, "preset", "veryfast", 0);
			//OpenFFmpeg::av_opt_set(cctx->priv_data, "crf", "23", 0);
			//OpenFFmpeg::av_opt_set(cctx->priv_data, "tune", "zerolatency", 0);

			if (OpenFFmpeg::avcodec_open2(cctx, codec, &codec_options) < 0)
			{
				FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::CameraCriticalError, "Could not open encoder video CODEC");
				return;
			}

			//Allocate a frame
			pFrame = OpenFFmpeg::av_frame_alloc();

			//allocate a picture object
			OpenFFmpeg::avpicture_alloc(&pic, OpenFFmpeg::AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
			//photoCtxt = OpenFFmpeg::sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, OpenFFmpeg::AV_PIX_FMT_BGR24, SWS_BICUBIC, NULL, NULL, NULL);
			photoCtxt = OpenFFmpeg::sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, OpenFFmpeg::AV_PIX_FMT_BGR24, SWS_SPLINE, NULL, NULL, NULL);
			if (photoCtxt == NULL)
			{
				return;
			}

			int ret;
			int got_picture;
			int got_output;
			video_frame_count = 0;

			time_t fpsStart;
			time_t fpsCurrent;
			time_t frameTimeStamp;

			//assign the current time 
			time(&fpsStart); 

			tm *ltm = localtime(&fpsStart);
			FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, String::Format("Camera Connected : {0}-{1}-{2} {3}:{4}:{5}", 1900 + ltm->tm_year, ltm->tm_mon+1,ltm->tm_mday, ltm->tm_hour, ltm->tm_min, ltm->tm_sec));

			double elapseTime = 0.0;
			double videoFPS = 0.0;
			int64_t iDen = 0;
			int64_t offset = 0;

  			while (!bStopRequested)
			{
				try
				{
					//Allocate packet
					OpenFFmpeg::AVPacket *packet = (OpenFFmpeg::AVPacket *) OpenFFmpeg::av_malloc(sizeof(OpenFFmpeg::AVPacket));
					
					//Reset timer of time out
					pFormatCtx->start_time = GetTickCount();

					// Set the call back before attempting to read the frame each time
					try
					{
						// George Sun
						// This is the audio input, add it to the audio buffer, and free the packet
						//ret = OpenFFmpeg::av_read_frame(pMicroPhoneCtx, packet);
						//pin_ptr<System::Byte> p = &audioBytes[audioByteIndex];// &tCVideoAudioMetaData->audioBufferBytes[0];
						//memcpy(p, packet->data, packet->size);
						//audioByteIndex += packet->size;
						//av_free_packet(packet);

						//Handel the read specially, because it is the main point of failure.
						ret = -1;
						ret = OpenFFmpeg::av_read_frame(pFormatCtx, packet);
					}
					catch (...)
					{
						FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, "AV_READ_FRAME failed");
					}

					if(ret >= 0)
					{
						//if it is video
						if(packet->stream_index == videoindex)
						{
							//Decode the video frame
							try
							{
								if (offset == 0)
								{
									offset = packet->pts;
								}
								ret = OpenFFmpeg::avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
								if (ret < 0)
								{
									FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, "AVCODEC_DECODE Failed");
									//return;
								}

								//Push the packet to Circular buffer, including background and main event recording buffer
								//George Sun
								// For Logitech, we need to encode to H264 before save it to BlackBox
								time(&frameTimeStamp); //assign the current time
								//if (bIsIPCamera)
								if (!bIsLogitech)
								{
									AddVideoAndAudio2CircularBuffer(packet, frameTimeStamp);
								}
								else
								{
									pkt = (OpenFFmpeg::AVPacket *) OpenFFmpeg::av_malloc(sizeof(OpenFFmpeg::AVPacket));
									OpenFFmpeg::av_init_packet(pkt); //pkt is the encoded output packet
									pkt->data = NULL;	//packet data will be allocated by the encoder
									pkt->size = 0;

									pFrame->pts = packet->pts - offset;
									pFrame->pts = iDen++;
									//pFrame->pict_type = OpenFFmpeg::AVPictureType::AV_PICTURE_TYPE_NONE;
									ret = OpenFFmpeg::avcodec_encode_video2(cctx, pkt, pFrame, &got_output);
									if (ret < 0)
										FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, "Error encoding frame");

									if (got_output)
									{
										//if (cctx->coded_frame->pts != AV_NOPTS_VALUE)
										{
											//pkt->pts = OpenFFmpeg::av_rescale_q(cctx->coded_frame->pts, cctx->time_base, pCodecCtx->time_base);
											//pkt->pts = OpenFFmpeg::av_rescale_q(packet->pts - offset, pCodecCtx->time_base, cctx->time_base);
											//pkt->pts = (packet->pts - offset) / 50;
											//pkt->pts = iDen++;
											//pkt->pts = pFrame->pts;
										}
										//fprintf(stderr, "Video Frame PTS: %d\n", (int)packet.pts);
										//}
										//else
										//{
										//fprintf(stderr, "Video Frame PTS: not set\n");
										//}
										//if (cctx->coded_frame->key_frame)
										//{
										//	pkt->flags |= AV_PKT_FLAG_KEY;
										//}

										//pkt->pts = iDen;
										AddVideoAndAudio2CircularBuffer(pkt, frameTimeStamp);
									}

									//George Sun
									// need to release the packet here, since it is not pushed into the blackbox
									av_free_packet(packet);
								}
							}
							catch (...)
							{
								FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::StringValue, String::Format("AVCODEC_DECODE Failed, {0}", pFormatCtx->start_time));
							}

							if(got_picture)
							{
								video_frame_count ++;
								ret = OpenFFmpeg::sws_scale(photoCtxt, pFrame->data, pFrame->linesize, 0, pFrame->height, pic.data, pic.linesize);
								if (ret > 0)
								{
									if (video_frame_count >= 60)	//when over 60 frame, start the calculation
									{
										time(&fpsCurrent); //assign the current time
										elapseTime = (double) (fpsCurrent - fpsStart);
										if (elapseTime > 2)
										{
											videoFPS = video_frame_count /elapseTime;
											video_frame_count = 0;
											time(&fpsStart); //reset current time

											//fire event
											FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::DoubleValue, videoFPS);
										}
									}

									PostImage2PictureControl(&pic, pFrame->width, pFrame->height);
								}
              				}
						}
						else
						{
							// This is the audio input, add it to the audio buffer, and free the packet
							//pin_ptr<System::Byte> p = &audioBytes[audioByteIndex];// &tCVideoAudioMetaData->audioBufferBytes[0];
							//memcpy(p, packet->data, packet->size);
							//audioByteIndex += packet->size;
							av_free_packet(packet);
							av_free(packet);
						}
						//Sleep(5);
					}
					else
					{
						FireEvent(FFmpegVideoLibrary::CameraLibraryEventType::CameraCriticalError, "AV_READ_FRAME failed, Check camera connection");
						//Thread::Sleep(20);
						//Exit Thread
						//break;
					}

					//JHE don't release here.  Release it in blackbox class after write or delete
					//av_free_packet(packet);
				}
				catch (...)
				{
					//JHE don't release here.  Release it in blackbox class after write or delete
					//av_free_packet(packet);
				}
			}
			
			//JHE don't release here.  Release it in blackbox class after write or delete
			//if (packet != NULL)
			//{
			//	av_free(packet);
			//}
			OpenFFmpeg::avcodec_close(cctx);
			OpenFFmpeg::av_free(cctx);
		}
		catch (...)
		{
		}
		finally
		{
			if (hDC != NULL)
			{
				ReleaseDC(NULL, hDC);
				DeleteDC(hDC);
				hDC = NULL;
			}

			//if (ghBM != NULL)
			//{
			//	DeleteObject(ghBM);
			//	ghBM = NULL;
			//}

			if (photoCtxt != NULL)
			{
				sws_freeContext(photoCtxt);
			}

			if (pFrame != NULL)
			{
				av_free(pFrame);
			}

			if (pCodecCtx != NULL)
			{
				avcodec_close(pCodecCtx);
			}

			if (pFormatCtx != NULL)
			{
				avformat_close_input(&pFormatCtx);
			}

		}

		//Clean up
		bThreadExited = true;
	}
#pragma endregion

	#pragma region Support functions

	void FFmpegVideo::PostImage2PictureControl(OpenFFmpeg::AVPicture *pic, int width, int height)
	{
		bool saveFrameForLiveStreaming = false;

		//Get the preview control size
		::GetClientRect((HWND)hPreviewWnd.ToPointer(), &rc);

		//Method 1
		//ghBM = CreateDIBitmap(hDC, (BITMAPINFOHEADER *) &bi, CBM_INIT, (void*)(pic->data[0]), (BITMAPINFO *)&bi, DIB_RGB_COLORS);
		//if (ghBM == NULL)
		//{
		//	return;
		//}
		//FFmpegVideoLibrary::FFmpegVideo::eNewFrameEventHandler(&((IntPtr) ghBM));

		//Method 2
		lineSize = pic->linesize[0];
		//padding = GetPadding(lineSize);

		if (framePixelsPtr == NULL)
		{
			framePixelsPtr = new uint8_t[height * (lineSize + padding)];
		}

		for (int32_t y = 0; y < height; ++y)
		{
			::CopyMemory(framePixelsPtr + (lineSize + padding) * y,	pic->data[0] + (height - y - 1) * lineSize, lineSize);
			::SecureZeroMemory(framePixelsPtr + (lineSize + padding) * y + lineSize, padding);
		}

	    //PAINTSTRUCT ps;
	    //HDC hdc = ::BeginPaint((HWND)hPreviewWnd.ToPointer(), &ps);

		//Get the DC of preview windows control
		hDC = ::GetDC((HWND)hPreviewWnd.ToPointer());

		// if liveStreaming, only save one frame every 6 frame
		if (IsVideoLiveStreaming)
		{
			if ((video_frame_count > 0) && (video_frame_count % 6) == 0)
			{
				saveFrameForLiveStreaming = true;
			}
		}

		if (takePhotoRequest || saveFrameForLiveStreaming)
		{
			CameraLibrarySaveFrameType eventType = GetSaveFrameType(takePhotoRequest, saveFrameForLiveStreaming);

			takePhotoRequest = false;
			SaveFrametoPhoto(pic, width, height, eventType);
		}

		::SetStretchBltMode(hDC, HALFTONE);
		::StretchDIBits(hDC, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0, 0, width, height, framePixelsPtr, &bmpInfo, DIB_RGB_COLORS, SRCCOPY);
	    //::EndPaint((HWND)hPreviewWnd.ToPointer(), &ps);   
		ReleaseDC(NULL, hDC);
		//DeleteDC(hDC);
	}

	void FFmpegVideo::SaveFrametoPhoto(OpenFFmpeg::AVPicture *pic, int width, int height, CameraLibrarySaveFrameType eventType)
	{
		try
		{
			if (bi.bV5Width <= 1)
			{
				bi.bV5Size = sizeof(BITMAPV5HEADER);
				bi.bV5Width = width;
				bi.bV5Height = -height;
				bi.bV5Planes = 1;
				bi.bV5BitCount = 24;
				bi.bV5Compression = BI_RGB;
				bi.bV5CSType = LCS_sRGB;
				bi.bV5Endpoints = CIEXYZTRIPLE();
				bi.bV5Intent = LCS_GM_IMAGES;
			}

			ghBM = CreateDIBitmap(hDC, (BITMAPINFOHEADER *)&bi, CBM_INIT, (void*)(pic->data[0]), (BITMAPINFO *)&bi, DIB_RGB_COLORS);
			if (ghBM == NULL)
			{
				return;
			}			

			FFmpegVideoLibrary::FFmpegVideo::eNewFrameEventHandler(eventType, &((IntPtr)ghBM));
		}
		catch (...)
		{

		}
	}

	void FFmpegVideo::AddVideoAndAudio2CircularBuffer(OpenFFmpeg::AVPacket * framePacket, time_t frameTimeStamp)
	{
		//if (isAudioBuffer)
		{
			if (audioByteIndex == 0 && circularBlackBox->lastAudioLen == 0)
			{
				System::Diagnostics::Trace::WriteLine("No audio Data received in streaks at " + frameTimeStamp.ToString() + ". Add 4096 bytes into the buffer.");
				audioByteIndex = 4096;
			}
//			else
				circularBlackBox->lastAudioLen = audioByteIndex;

			//System::Diagnostics::Trace::WriteLine(frameTimeStamp.ToString() + ": Audio Data Saved = " + audioByteIndex.ToString());

			array<Byte>^ audioData = gcnew array<Byte>(audioByteIndex);
			Array::Copy(audioBytes, audioData, audioByteIndex);

			CVideoAudioMetaData ^ newMetaDataPacket = gcnew CVideoAudioMetaData(framePacket, audioData, audioByteIndex, frameTimeStamp);
			circularBlackBox->AddVideoAudioFrame(newMetaDataPacket);
			
			audioByteIndex = 0;
		}
		//else
		//{
			//CVideoAudioMetaData ^ newMetaDataPacket = gcnew CVideoAudioMetaData(framePacket, nullptr, 0, frameTimeStamp);
			//circularBlackBox->AddVideoAudioFrame(newMetaDataPacket);
		//}
	}

	int FFmpegVideo::CurrentPreEventLength()
	{
		if (circularBlackBox != nullptr)
		{
			return circularBlackBox->CurrentPreEventLength;
		}

		return 30;
	}
	//void FFmpegVideo::FireEvent(double dValue)
	//{
	//	//Trigger event for int
	//	FFmpegVideoLibrary::FFmpegVideo::eNewNumericValueEventHandler(dValue);
	//}

	//void FFmpegVideo::FireEvent(String ^ strTxt)
	//{
	//	//Trigger event for String
	//	FFmpegVideoLibrary::FFmpegVideo::eNewStringEventHandler(strTxt);
	//}

	void FFmpegVideo::FireEvent(CameraLibraryEventType actionType, Object^ args)
	{
		//Trigger event for action event
		FFmpegVideoLibrary::FFmpegVideo::eNewActionEventHandler(actionType, args);
	}

	char* FFmpegVideo::ManagedStringToUnmanagedUTF8Char(String^ str)
	{
		pin_ptr<const wchar_t> wch = PtrToStringChars(str);
		int nBytes = ::WideCharToMultiByte(CP_UTF8, NULL, wch, -1, NULL, 0, NULL, NULL);
		char* lpszBuffer = new char[nBytes];
		ZeroMemory(lpszBuffer, (nBytes) * sizeof(char)); 
		nBytes = ::WideCharToMultiByte(CP_UTF8, NULL, wch, -1, lpszBuffer, nBytes, NULL, NULL);
		return lpszBuffer;
	}

	CameraLibrarySaveFrameType FFmpegVideo::GetSaveFrameType(bool takePhotoFlag, bool liveStreamFlag)
	{
		CameraLibrarySaveFrameType eventType = CameraLibrarySaveFrameType::None;
		if (takePhotoFlag && liveStreamFlag)
		{
			eventType = CameraLibrarySaveFrameType::Both;
		}
		else if (takePhotoFlag)
		{
			eventType = CameraLibrarySaveFrameType::TakePhoto;
		}
		else if (liveStreamFlag)
		{
			eventType = CameraLibrarySaveFrameType::LiveStream;
		}

		return eventType;
	}
	#pragma endregion
}
