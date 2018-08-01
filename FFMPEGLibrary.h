// FFMPEGLibrary.h
#pragma once
#include <windows.h>
#include "BlackBox.h"

using namespace System;
using namespace System::Threading;
using namespace System::Drawing;

using namespace NAudio::Wave;

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

namespace FFmpegVideoLibrary 
{
	public enum class CameraLibraryEventType { VideoSaved, LiveVideo, CameraCriticalError, CameraModeChange, CameraExposureChange, StringValue, DoubleValue};
	public enum class CameraLibrarySaveFrameType { TakePhoto, LiveStream, Both, None };

	public ref class FFmpegVideo : IDisposable
	{
		// TODO: Add your methods for this class here.
	#pragma region Class structure and member variables
		public:
			static OpenFFmpeg::AVCodecContext*  pCodecCtx;

			FFmpegVideo(void)
			{
				InitialLize();
			}

			~FFmpegVideo()
			{
				this->!FFmpegVideo();
			}

			!FFmpegVideo()
			{
				InitialLizeAudioMonitor(false);

				if (framePixelsPtr != NULL)
				{
					delete framePixelsPtr;
				}

				delete circularBlackBox;
			}

		private:
			//Class control variables
			static bool bStopRequested = false;
			static bool bThreadExited = false;
			static bool isConnected = false;

			//Camera properties and variables
			static IntPtr hPreviewWnd;
			static String^ cameraConnectionPath;
			static String^ MicroPhoneConnectionPath;
			static HDC hDC;
			static bool bIsIPCamera = true;
			//George Sun
			// Add a flag of the Logitech camera
			static bool bIsLogitech = true;

			//Video properties and variables
			static int iSkyBrightness = 0;
			static Size videoFrameSize = Size(1280, 720);

			//Circular Buffer Length, seconds
			int iCircularBufferLength;	//in seconds, default to 30 seconds

			//uint32_t GetPadding(int32_t lineSize)
			//{
			//	return lineSize % sizeof(uint32_t) > 0 ?
			//		sizeof(uint32_t) - (lineSize % sizeof(uint32_t)) : 0;
			//}
			int32_t lineSize;
			static uint32_t padding = 0;
			uint8_t * framePixelsPtr;
			int video_frame_count;

			//Audio input device properties and variables
			String ^ audioInputDeviceName;
			WaveIn ^ frontAudioWaveIn;

			//Circular buffer properties
			static CBlackBox ^ circularBlackBox;
			int iVideoFileChumkLength;
			static String ^ videoRecordRootFilePath;

			bool takePhotoRequest;
			bool isVideoLiveStreaming;

			static bool isAudioBuffer;
			static int MAX_AUDIO_PACKET_SIZE = 4194304; //1024*1024*4
			static Object^ globalAudioDataBufferLock;
			static array<Byte>^ audioBytes;
			static int audioByteIndex = 0;

	#pragma endregion

	#pragma region Class Properties
		public:
			property bool IsConnected
			{
				bool get()
				{
					return isConnected;
				}
			}

			property bool IsIPCamera
			{
				bool get()
				{
					return bIsIPCamera;
				}
				void set(bool value)
				{
					bIsIPCamera = value;
				}
			}

			//George Sun
			property bool IsLogitech
			{
				bool get()
				{
					return bIsIPCamera;
				}
				void set(bool value)
				{
					bIsIPCamera = value;
				}
			}

			property IntPtr VideoPreviewWindowHandle
			{
				void set(IntPtr value)
				{
					hPreviewWnd = value;
				}
			}

			property Size FrameSize
			{
				void set(Size value)
				{
					videoFrameSize = value;
				}
	 		}

			property int FrameRate
			{
				void set(int value)
				{
				}

				int get()
				{
					return 30;
				}
	 		}

			property int VideoPreeventLength
			{
				void set(int value)
				{
					iCircularBufferLength = value;
				}

				int get()
				{
					return iCircularBufferLength;
				}
			}

			property int SkyBrightness
			{
				int get()
				{
					return iSkyBrightness;
				}
			}

			property String^ CameraConnectionPath
			{
				String^ get()
				{
					return cameraConnectionPath;
				}
				void set(String^ value)
				{
					cameraConnectionPath = value;
				}
			}

			property String^ AudioInputDeviceName
			{
				String^ get()
				{
					return audioInputDeviceName;
				}
				void set(String^ value)
				{
					audioInputDeviceName = value;
				}
			}

			property int DefaultVideoChuck
			{
				int get()
				{
					return iVideoFileChumkLength;
				}
				void set(int value)
				{
					iVideoFileChumkLength = value;
				}
			}

			property String^ VideoRecordRootFilePath
			{
				String^ get()
				{
					return videoRecordRootFilePath;
				}
				void set(String^ value)
				{
					videoRecordRootFilePath = value;
				}
			}

			property bool IsVideoLiveStreaming
			{
				bool get()
				{
					return isVideoLiveStreaming;
				}
				void set(bool value)
				{
					isVideoLiveStreaming = value;
				}
			}

	#pragma endregion

	#pragma region Class Functions
		public:
			//Library controls
			void Connect();
			void Disconnect();

			//Video functions
			void StartMainEventRecord(String^ videoFileName);
			void StopMainEventRecord();
			void StartNewChuckVideoRecording(String^ videoFileName);

			void StartBackgroundRecord();
			void StopBackgroundRecord();

			void TakePhotoImageAction();
			//delegate void NewNumericValueEventHandler(double dValue);
			//static event NewNumericValueEventHandler ^ eNewNumericValueEventHandler;

			//delegate void NewStringEventHandler(String ^ strTxt);
			//static event NewStringEventHandler ^ eNewStringEventHandler;

			delegate void NewActionEventHandler(CameraLibraryEventType actionType, Object^ args);
			static event NewActionEventHandler ^ eNewActionEventHandler;

			delegate void NewFrameEventHandler(CameraLibrarySaveFrameType eventType, IntPtr * pBuffer);
			event NewFrameEventHandler ^ eNewFrameEventHandler;

			//static void FireEvent(double dValue);
			//static void FireEvent(String ^ strTxt);
			static void FireEvent(CameraLibraryEventType actionType, Object^ args);

			static char* ManagedStringToUnmanagedUTF8Char(String^ str);
			int CurrentPreEventLength();

		private:
			void InitialLize();

			void InitialLizeAudioMonitor(bool startMonitoring);
			void OnDataAvailable(System::Object ^sender, NAudio::Wave::WaveInEventArgs ^e);
			int GetCodecWaveInAudioID(String ^ audioDeviceName, int iStartIndex);

			void videoFrameThreadProc();
			void PostImage2PictureControl(OpenFFmpeg::AVPicture *pic, int width, int height);

			void AddVideoAndAudio2CircularBuffer(OpenFFmpeg::AVPacket * framePacket, time_t frameTimeStamp);

			void SaveFrametoPhoto(OpenFFmpeg::AVPicture *pic, int width, int height, CameraLibrarySaveFrameType eventType);

			CameraLibrarySaveFrameType GetSaveFrameType(bool takePhotoFlag, bool liveStreamFlag);

#pragma endregion
	};
}
