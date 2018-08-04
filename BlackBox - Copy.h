#pragma once
#include "VideoAudioMetaData.h"
namespace OpenFFmpeg
{
	extern "C"
	{
		#include <libavcodec/avcodec.h>
		#include <libavformat/avformat.h>
		#include <libswscale/swscale.h>
		#include <libavdevice/avdevice.h>
	}
}

using namespace System;
using namespace System::Collections::Concurrent;
using namespace System::Threading;
using namespace System::Drawing;

public enum class VideoEncoding { MPEG4, H264 };

public ref class ThreadParams
{
	public:
		String^ videoFileName;
		int frameRate;
		int bitRate;
	
		ThreadParams(String^ pVideoFileName, int pFrameRate, int pBitRate)
		{
			videoFileName = pVideoFileName;
			frameRate = pFrameRate;
			bitRate = pBitRate;
		}
};

public ref class CBlackBox
{
	#pragma region Class structure and member variables
	public:

		int iVideoFileChumkLength;	//in minutes, default to 20 minutes
		int lastAudioLen;
	
		CBlackBox(void);
		CBlackBox(bool isIPCamera);

		~CBlackBox()
		{
			// Free any other managed objects here.
			//Stop buffering archive
			enable24HrsRecording = false;

			//Stop archive
			bStopBackgroundArchive = true;

			//Stop recording
			bStopRecording = true;

			Thread::Sleep(400);

			//release all resources
			while (!videoMainCircularBufferQueue->IsEmpty)
			{
				CVideoAudioMetaData ^ tCVideoAudioMetaData;
				videoMainCircularBufferQueue->TryDequeue(tCVideoAudioMetaData);

				OpenFFmpeg::av_packet_unref(tCVideoAudioMetaData->videoPacket);
				OpenFFmpeg::av_free(tCVideoAudioMetaData->videoPacket);
				delete tCVideoAudioMetaData;
			}

			while (!videobackgroundCircularBufferQueue->IsEmpty)
			{
				CVideoAudioMetaData ^ tCVideoAudioMetaData;
				videobackgroundCircularBufferQueue->TryDequeue(tCVideoAudioMetaData);

				delete tCVideoAudioMetaData;
			}

			delete dtStartBuffering;
		};

	private:	
		int eclipeTotalSeconds;
		int iBrightness;
		int iBrightnessSky;
		bool bIsIPCamera;
		bool bIsLogitech;

		static bool commitAll = false;
		static bool bStopRecording = false;
		static bool bStopBackgroundArchive = true;

		//VideoEncoding videoEncoding = VideoEncoding::MPEG4;

		void Initialize();
	#pragma endregion
	

#pragma region Class Properties
		public:
			property bool StopVideoArchive
			{
				bool get()
				{
					return bStopBackgroundArchive;
				}
				void set(bool value)
				{
					bStopBackgroundArchive = value;
				}
			}

			property bool StopVideoRecording
			{
				bool get()
				{ 
					return bStopRecording; 
				}
				void set(bool value)
				{
					bStopRecording = value;
				}
			}

			property int Brightness
			{
				int get()
				{
					return iBrightness;
				}
				void set(int value)
				{
					iBrightness = value;
				}
			}

			property int BrightnessSky
			{
				int get()
				{
					return iBrightnessSky;
				}
				void set(int value)
				{
					iBrightnessSky = value;
				}
			}


#pragma endregion

#pragma region Video Recording
		public:
			//property
			property int VideoPreEventLength
			{
				int get()
				{ 
					return iCircularBufferLength; 
				}
				void set(int value)
				{
					iCircularBufferLength = value;
				}
			}

			property int CurrentPreEventLength
			{
				int get()
				{
					return (int) (time(0) - *dtStartBuffering);
				}
			}

			property bool SavingVideoToFile
			{
				bool get()
				{ 
					return commitAll; 
				}
			}

			property bool IsRecording
			{
				bool get()
				{ 
					return isRecordingStarted; 
				}
				void set(bool value)
				{
					isRecordingStarted = value;
				}
			}

			property Size VideoFrameSize
			{
				Size get()
				{ 
					return sizeofFrame; 
				}
				void set(Size value)
				{
					if (sizeofFrame != value)
					{
						//ResetCircularBuffer(); //??
					}

					sizeofFrame = value;
				}
			}

			property int FrameRate
			{
				int get()
				{ 
					return iFrameRate; 
				}
				void set(int value)
				{
					iFrameRate = value;
				}
			}

			//class functions
			bool StartRecording(String^ videoFileName, int frameRate, int bitRate);
			bool StartNewChuckVideoRecording(String^ newVideoFileName);
		private:
			ConcurrentQueue <CVideoAudioMetaData^> ^videoMainCircularBufferQueue;
			
			//Circular Buffer Length, seconds
			int iCircularBufferLength;	//in seconds, default to 30 seconds

			//default frame Size
			Size sizeofFrame;

			//default frame rate
			int iFrameRate;

			//Indicator of buffering
			bool isRecordingStarted;

			bool isStartNewChuckVideoRecording;
			String^ newChuckVideoFileName;
			time_t ^ dtStartBuffering;

			void MainVideoWriterThreadProc(Object^ paramObj);
#pragma endregion

#pragma region 24 Hours Video Archive
	public:
		//property
		property bool Enable24HrsRecording
		{
			bool get()
			{ 
				return enable24HrsRecording; 
			}
		void set(bool value)
			{ 
				enable24HrsRecording = value; 
			}
		}

		property int DefaultVideoChuck
		{
			int get()
			{ 
				return iVideoFileChumkLength / 60;
			}
			void set(int value)
			{ 
				iVideoFileChumkLength = value * 60;
			}
		}

		property String^ DefaultVideoFileRootFolder
		{
			String^ get()
			{ 
				return defaultFileRootFolder; 
			}
			void set(String^ value) 
			{ 
				defaultFileRootFolder = value; 
			}
		}

		property String^ RecordFileDateTimeFormat
		{
			String^ get()
			{ 
				return recordFileDateTimeFormat; 
			}
			void set(String^ value)
			{ 
				recordFileDateTimeFormat = value; 
			}
		}

		//class functions
		void Start24HrsVideoArchive();
		void Stop24HrsVideoArchive();
	private:
		ConcurrentQueue <CVideoAudioMetaData^> ^videobackgroundCircularBufferQueue;
		void backgroundVideoWriterThreadProc();

		bool enable24HrsRecording;

		//default archive frame Size
		//Size archSizeofFrame;

		//Default archive frame rate
		//int archVideoFrameRate;

		//Default archive bitrate
		//int archVideobitRate;

		String ^defaultFileRootFolder;
		/*static bool isArchiveVideoStarted = false;*/
		String ^recordFileDateTimeFormat;

		//class functions
		void ResetArchiveCircularBuffer();
		//void RefreshArchiveFFMPEGWriter();
		//void ArchiveVideoToFile();

#pragma endregion

#pragma region Video Recording
	public:
	bool CommitAll();
	void Reset();

	private:

#pragma endregion

#pragma region Event Notification
	// Invoke the Radar data event; called whenever it comes
	public:
		void AddVideoAudioFrame(CVideoAudioMetaData ^ tCVideoAudioMetaData);
		void FireVideoSavedEvent(String^ videoFileName);
		
#pragma endregion

};
