#include "workthread.h"
#include "globalconfig.h"
#include "devicecontrol.h"
#include "opencv2/opencv.hpp"

//必须加以下内容,否则编译不能通过,为了兼容C和C99标准
#ifndef INT64_C
#define INT64_C
#define UINT64_C
#endif

//引入ffmpeg头文件
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

using namespace cv;

#define TIMEMS QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")

WorkThread::WorkThread(QString VideoName, quint32 width, quint32 height, quint32 BPP, quint32 pixelformat, QObject *parent) :
    QThread(parent)
{
    this->VideoName = VideoName;
    this->width = width;
    this->height = height;

    this->isStopCapture = false;
    this->isReInitialize = false;
    this->isVaild = false;
    this->isAlarm = false;

    rgb_buff = (uchar*)malloc(width * height * 24 / 8);

    operatecamera = new OperateCamera(VideoName,width,height,BPP,pixelformat,this);
    if(operatecamera->OpenCamera()){
        if(operatecamera->InitCameraDevice()){
            qDebug() << TIMEMS << VideoName << "success in VIDIOC_STREAMON";
            this->isVaild = true;
        }
    }
}

void WorkThread::run()
{
    QElapsedTimer timer;

    while(1){
        while(!isStopCapture){
            timer.start();

            QImage image = ReadFrame();
            if(!image.isNull()){
                ProcessFrame(image);
            }

            int diff = timer.elapsed();
            qDebug() << TIMEMS << VideoName <<  diff << "ms";

            msleep(GlobalConfig::CameraSleepTime);
        }
        msleep(100);
    }
}


QImage WorkThread::ReadFrame()
{
    if(isReInitialize){
        operatecamera->CleanupCaptureDevice();
        if(operatecamera->OpenCamera()){
            if(operatecamera->InitCameraDevice()){
                this->isVaild = true;
                this->isReInitialize = false;
                emit signalUSBCameraOnline();
                qDebug() << TIMEMS << VideoName << "success in VIDIOC_STREAMON";
            }else{
                this->isVaild = false;
                qDebug() << TIMEMS << VideoName << "failed in VIDIOC_STREAMON";
            }
        }else{
            this->isVaild = false;
        }
    }

    if(!isVaild){
        this->isStopCapture = true;
        emit signalUSBCameraOffline();
        return QImage();
    }

    if(operatecamera->ReadFrame()){
        if(YUYVToRGB24_FFmpeg(operatecamera->yuyv_buff, rgb_buff)){
            QImage image_rgb888((quint8 *)rgb_buff, this->width, this->height, QImage::Format_RGB888);
            NormalImage = image_rgb888;
            return image_rgb888;
        }
    }else{        
        qDebug() << TIMEMS << VideoName << "Usb Camera Error happen in ReadFrame";
        this->isStopCapture = true;
        emit signalUSBCameraOffline();
        return QImage();
    }
}

void WorkThread::ProcessFrame(QImage &image)
{
    if(!this->SelectRect.isEmpty() && !this->LightPoint.isEmpty()){
        qDebug() << TIMEMS << VideoName << "ProcessFrame";
        IplImage *pSrcImage = cvCreateImageHeader(cvSize(image.width(),image.height()),
                                                  IPL_DEPTH_8U,3);
        pSrcImage->imageData = (char*)image.bits();
        cvConvertImage(pSrcImage,pSrcImage,CV_CVTIMG_SWAP_RB);

        //截图
        CvSize size= cvSize(this->SelectRect.width(),this->SelectRect.height());//区域大小
        cvSetImageROI(pSrcImage,cvRect(this->SelectRect.left(),this->SelectRect.top(),this->SelectRect.width(),this->SelectRect.height()));//设置源图像ROI
        IplImage *pImageRoi = cvCreateImage(size,pSrcImage->depth,pSrcImage->nChannels);//创建目标图像
        cvCopy(pSrcImage,pImageRoi); //复制图像
        cvResetImageROI(pSrcImage);//源图像用完,清空ROI

        //创建图像并缩放
        CvSize ReSize;
        ReSize.width = pImageRoi->width * this->factor;
        ReSize.height = pImageRoi->height * this->factor;
        IplImage *pResizeImage = cvCreateImage(ReSize, pImageRoi->depth, pImageRoi->nChannels);
        cvResize(pImageRoi, pResizeImage, CV_INTER_AREA);

        // 转为灰度图
        IplImage *pGrayImage =  cvCreateImage(cvGetSize(pResizeImage), IPL_DEPTH_8U, 1);
        cvCvtColor(pResizeImage, pGrayImage, CV_BGR2GRAY);

        // 创建二值图
        IplImage *pBinaryImage = cvCreateImage(cvGetSize(pGrayImage), IPL_DEPTH_8U, 1);

        // 转为二值图
        cvThreshold(pGrayImage, pBinaryImage, 220, 255, CV_THRESH_BINARY);

        // 检索轮廓
        CvMemStorage *pcvMStorage = cvCreateMemStorage();
        CvSeq *pcvSeq = NULL;
        cvFindContours(pBinaryImage, pcvMStorage, &pcvSeq, sizeof(CvContour), CV_RETR_TREE, CV_CHAIN_APPROX_SIMPLE, cvPoint(0, 0));

        //判断轮廓是否包含设定的点
        qint8 LightPointNumber = 0;

        for (CvSeq *pContour = pcvSeq; pContour != NULL; pContour = pContour->h_next){
            //取得轮廓面积
//            double contArea = fabs(cvContourArea(pContour,CV_WHOLE_SEQ));

            for(qint8 i = 0; i < this->LightPoint.size(); i++){
                if(cvPointPolygonTest(pContour,cvPoint2D32f(this->LightPoint.at(i).x(),this->LightPoint.at(i).y()),0) == 1){
                    LightPointNumber++;
                }
            }
        }

        if(LightPointNumber != this->LightPoint.size()){
            if(!isAlarm){
                isAlarm = true;
                //报警操作
                cvConvertImage(pSrcImage,pSrcImage,CV_CVTIMG_SWAP_RB);
                AlarmImage = image;
                emit signalAlarmImage();
            }
        }else{
            isAlarm = false;
        }

        //报警操作
//        cvConvertImage(pSrcImage,pSrcImage,CV_CVTIMG_SWAP_RB);
//        AlarmImage = image;
//        emit signalAlarmImage();

        cvReleaseImage(&pSrcImage);
        cvReleaseImage(&pImageRoi);
        cvReleaseImage(&pResizeImage);
        cvReleaseImage(&pGrayImage);
        cvReleaseImage(&pBinaryImage);
        cvReleaseMemStorage(&pcvMStorage);
    }
}

bool WorkThread::YUYVToRGB24_FFmpeg(uchar *pYUV,uchar *pRGB24)
{
    if (width < 1 || height < 1 || pYUV == NULL || pRGB24 == NULL){
        return false;
    }

    AVPicture pFrameYUV,pFrameRGB;
    avpicture_fill(&pFrameYUV,pYUV,AV_PIX_FMT_YUYV422,width,height);
    avpicture_fill(&pFrameRGB,pRGB24,AV_PIX_FMT_RGB24,width,height);

    //U,V互换
    uint8_t *ptmp = pFrameYUV.data[1];
    pFrameYUV.data[1] = pFrameYUV.data[2];
    pFrameYUV.data [2] = ptmp;

    struct SwsContext *imgCtx =
            sws_getContext(width,height,AV_PIX_FMT_YUYV422,width,height,AV_PIX_FMT_RGB24,SWS_BILINEAR,0,0,0);

    if(imgCtx != NULL){
        sws_scale(imgCtx,pFrameYUV.data,pFrameYUV.linesize,0,height,pFrameRGB.data,pFrameRGB.linesize);
        if(imgCtx){
            sws_freeContext(imgCtx);
            imgCtx = NULL;
        }
        return true;
    }else{
        sws_freeContext(imgCtx);
        imgCtx = NULL;
        return false;
    }
}

void WorkThread::Clear()
{
    operatecamera->CleanupCaptureDevice();
    free(operatecamera->yuyv_buff);
    free(this->rgb_buff);
}
