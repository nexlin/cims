/*
 * Copyright (C) 2021 Teluu Inc. (http://www.teluu.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
package org.pjsip;

import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Range;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.SurfaceHolder;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

import org.pjsip.PjCameraInfo2;

public class PjCamera2
{
    private final String TAG = "PjCamera2";

    private CameraDevice camera = null;
    private CameraCaptureSession previewSession = null;

    private boolean isRunning = false;
    private boolean start_with_fps = true;
    private int camIdx;
    private final long userData;
    private final int fps;
    private final int w;
    private final int h;
    private final int fmt;

    private ImageReader imageReader;
    private HandlerThread handlerThread = null;
    private Handler handler;

    /* For debugging purpose only */
    private final SurfaceView surfaceView;

    /* 로컬 셀프뷰(내 화면) 프리뷰 surface. 앱이 SetPreviewSurface() 로 등록한다.
     * 카메라를 다시 열지 않고, 이미 열린 CameraDevice 에 CaptureSession 만
     * 재구성해 인코딩용 ImageReader 와 함께 출력 target 으로 추가한다
     * (Camera2 는 한 세션에서 다중 출력 surface 지원 → 카메라 2중 오픈/SYSERR 회피). */
    private Surface previewSurface = null;
    private static PjCamera2 active = null;
    private static Surface pendingPreview = null;

    /** 앱(SipController)에서 셀프뷰 surface 등록/해제. null 이면 프리뷰 제거.
     * surfaceCreated/surfaceChanged 가 같은 Surface 로 여러 번 콜백하므로 참조 동일 시 무시한다
     * (반복 세션 재구성이 카메라 teardown race 를 유발하기 때문). */
    public static void SetPreviewSurface(Surface s)
    {
        if (s == pendingPreview)
            return;
        pendingPreview = s;
        PjCamera2 a = active;
        if (a != null)
            a.applyPreviewSurface(s);
    }

    /** 실행 중 카메라의 CaptureSession 을 프리뷰 surface 포함/제외로 재구성. */
    private void applyPreviewSurface(final Surface s)
    {
        final Handler h = handler;
        if (h == null || camera == null) {
            previewSurface = s;
            return;
        }
        h.post(new Runnable() {
            @Override
            public void run() {
                // 이미 반영됐거나(중복) 더 최신 요청이 왔으면(stale) 재구성 생략 — teardown race 방지.
                if (s == previewSurface || s != pendingPreview)
                    return;
                previewSurface = s;
                if (!isRunning || camera == null)
                    return;
                try {
                    if (previewSession != null) {
                        previewSession.close();
                        previewSession = null;
                    }
                    StartPreview();
                } catch (Exception e) {
                    Log.d(TAG, "applyPreviewSurface: " + e.getMessage());
                }
            }
        });
    }

    native void PushFrame2(long userData_,
                           ByteBuffer plane0, int rowStride0, int pixStride0,
                           ByteBuffer plane1, int rowStride1, int pixStride1,
                           ByteBuffer plane2, int rowStride2, int pixStride2);

    ImageReader.OnImageAvailableListener imageAvailListener = new ImageReader.OnImageAvailableListener() {
        @Override
        public void onImageAvailable(ImageReader reader) {
            if (!isRunning)
                return;

            Image image = reader.acquireLatestImage();
            if (image == null)
                return;

            /* Get planes buffers. According to the docs, the buffers are always direct buffers */
            Image.Plane[] planes = image.getPlanes();
            ByteBuffer plane0 = planes[0].getBuffer();
            ByteBuffer plane1 = planes.length > 1 ? planes[1].getBuffer() : null;
            ByteBuffer plane2 = planes.length > 2 ? planes[2].getBuffer() : null;
            assert plane0.isDirect();

            //for (Image.Plane p: planes) {
            //  Log.d(TAG, String.format("size=%d bytes, getRowStride()=%d getPixelStride()=%d", p.getBuffer().remaining(), p.getRowStride(), p.getPixelStride()));
            //}

            PushFrame2( userData,
                        plane0, planes[0].getRowStride(), planes[0].getPixelStride(),
                        plane1, plane1!=null? planes[1].getRowStride():0, plane1!=null? planes[1].getPixelStride():0,
                        plane2, plane2!=null? planes[2].getRowStride():0, plane2!=null? planes[2].getPixelStride():0);

            image.close();
        }
    };

    private final CameraDevice.StateCallback camStateCallback = new CameraDevice.StateCallback() {
        @Override
        public void onOpened(CameraDevice c) {
            Log.i(TAG, "CameraDevice.StateCallback.onOpened");
            camera = c;
            StartPreview();
        }
        @Override
        public void onClosed(CameraDevice c) {
            Log.i(TAG, "CameraDevice.StateCallback.onClosed");
        }
        @Override
        public void onDisconnected(CameraDevice c) {
            Log.i(TAG, "CameraDevice.StateCallback.onDisconnected");
            Stop();
        }
        @Override
        public void onError(CameraDevice c, int error) {
            Log.e(TAG, "CameraDevice.StateCallback.onError: " + error);

            boolean was_with_fps = start_with_fps;
            Stop();

            /* First retry */
            if ((error == CameraDevice.StateCallback.ERROR_CAMERA_DEVICE ||
                 error == CameraDevice.StateCallback.ERROR_CAMERA_SERVICE) &&
                was_with_fps)
            {
                Log.i(TAG, "Retrying without enforcing frame rate..");
                start_with_fps = false;
                Start();
            }
        }
    };

    private final SurfaceHolder.Callback surfaceHolderCallback = new SurfaceHolder.Callback() {
        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            Log.d(TAG, "SurfaceHolder.Callback.surfaceCreated");
            if (camera != null) {
                StartPreview();
            }
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder,
                                   int format, int width, int height)
        {
            Log.d(TAG, "SurfaceHolder.Callback.surfaceChanged");
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder)
        {
            Log.d(TAG, "SurfaceHolder.Callback.surfaceDestroyed");
        }
    };

    public PjCamera2(int idx, int w_, int h_, int fmt_, int fps_,
                    long userData_, SurfaceView surface)
    {
        camIdx = idx;
        w = w_;
        h = h_;
        fmt = fmt_;
        userData = userData_;
        fps = fps_;
        surfaceView = surface;
    }

    public int SwitchDevice(int idx)
    {
        boolean isCaptureRunning = isRunning;
        int oldIdx = camIdx;

        if (isCaptureRunning)
            Stop();

        camIdx = idx;

        if (isCaptureRunning) {
            int ret = Start();
            if (ret != 0) {
                /* Try to revert back */
                camIdx = oldIdx;
                Start();
                return ret;
            }
        }

        return 0;
    }

    private void StartPreview()
    {
        try {
            List<Surface> surfaceList = new ArrayList<>();
            surfaceList.add(imageReader.getSurface());
            if (surfaceView != null)
                surfaceList.add(surfaceView.getHolder().getSurface());
            final Surface pv = previewSurface;
            if (pv != null && pv.isValid())
                surfaceList.add(pv);

            camera.createCaptureSession(surfaceList,
                new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession session) {
                        Log.d(TAG, "CameraCaptureSession.StateCallback.onConfigured");

                        // 재구성 race 로 이미 닫혔거나(camera==null) 정지 중이면 이 세션은 버린다.
                        // (여기서 Stop() 을 부르면 카메라 전체가 teardown 되어 셀프뷰가 사라짐)
                        if (camera == null || !isRunning) {
                            try { session.close(); } catch (Exception ignore) {}
                            return;
                        }
                        try {
                            CaptureRequest.Builder previewBuilder = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
                            previewBuilder.addTarget(imageReader.getSurface());
                            if (surfaceView != null)
                                previewBuilder.addTarget(surfaceView.getHolder().getSurface());
                            if (pv != null && pv.isValid())
                                previewBuilder.addTarget(pv);
                            previewBuilder.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO);
                            if (start_with_fps) {
                                Range<Integer> fpsRange = new Range<>(fps, fps);
                                previewBuilder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange);
                            }
                            session.setRepeatingRequest(previewBuilder.build(), null, handler);
                            previewSession = session;
                        } catch (Exception e) {
                            Log.d(TAG, "onConfigured: " + e.getMessage());
                            try { session.close(); } catch (Exception ignore) {}
                        }
                        if (surfaceView!=null)
                            surfaceView.getHolder().addCallback(surfaceHolderCallback);
                    }
                    @Override
                    public void onConfigureFailed(CameraCaptureSession session) {
                        Log.e(TAG, "CameraCaptureSession.StateCallback.onConfigureFailed");
                        Stop();
                    }
                }, handler);
        } catch (Exception e) {
            Log.d(TAG, e.getMessage());
            Stop();
        }
    }

    public int Start()
    {
        PjCameraInfo2 ci = PjCameraInfo2.GetCameraInfo(camIdx);
        if (ci == null) {
            Log.e(TAG, "Invalid device index: " + camIdx);
            return -1;
        }

        CameraManager cm = PjCameraInfo2.GetCameraManager();
        if (cm == null)
            return -2;

        handlerThread = new HandlerThread("Cam2HandlerThread");
        handlerThread.start();
        handler = new Handler(handlerThread.getLooper());

        /* Some say to put a larger maxImages to improve FPS */
        imageReader = ImageReader.newInstance(w, h, fmt, 3);
        imageReader.setOnImageAvailableListener(imageAvailListener, handler);
        isRunning = true;
        active = this;
        previewSurface = pendingPreview;    /* 통화 시작 전 등록된 셀프뷰 surface 승계 */

        try {
            cm.openCamera(ci.id, camStateCallback, handler);
        } catch (Exception e) {
            Log.d(TAG, e.getMessage());
            Stop();
            return -10;
        }

        return 0;
    }

    public void Stop()
    {
        if (!isRunning)
            return;

        isRunning = false;
        if (active == this)
            active = null;
        Log.d(TAG, "Stopping..");

        if (previewSession != null) {
            previewSession.close();
            previewSession = null;
        }

        if (camera != null) {
            camera.close();
            camera = null;
        }

        if (surfaceView != null)
            surfaceView.getHolder().removeCallback(surfaceHolderCallback);

        if (handlerThread != null) {
            handlerThread.quitSafely();
            try {
                if (handlerThread.getId() != Thread.currentThread().getId()) {
                    Log.d(TAG, "Wait thread..");
                    handlerThread.join();
                    Log.d(TAG, "Wait thread done");
                }
                handlerThread = null;
                handler = null;
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
        }

        if (imageReader != null) {
            imageReader.close();
            imageReader = null;
        }

        /* Reset setting */
        start_with_fps = true;

        Log.d(TAG, "Stopped.");
    }

}
