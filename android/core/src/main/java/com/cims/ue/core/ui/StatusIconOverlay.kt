package com.cims.ue.core.ui

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.PixelFormat
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.provider.Settings
import android.view.Gravity
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.ImageView

/**
 * 화면 최상단(상태바 바로 아래) 전역 상태 아이콘 배지 — 어떤 앱 위에서든 보인다.
 * CIMS-Phone(전화 아이콘)·McPTT(PTT 아이콘) 공용: **아이콘만** 표시하고 상태는 tint 색으로
 * 나타낸다(등록됨=초록/연결 중=황색/해제=회색/실패=적색). 두 앱이 동시에 떠도 겹치지 않게
 * [xOffsetDp] 로 중앙 기준 좌우 자리를 나눈다(Phone=-22, PTT=+22).
 *
 * 시스템 상태바 자체는 서드파티가 그릴 수 없으므로 `TYPE_APPLICATION_OVERLAY`
 * ('다른 앱 위에 표시' 권한, [Settings.canDrawOverlays]) 로 그 바로 아래에 붙인다.
 * 터치는 통과(FLAG_NOT_TOUCHABLE). 모든 호출은 main 스레드에서.
 */
class StatusIconOverlay(
    private val ctx: Context,
    private val iconRes: Int,
    private val xOffsetDp: Int = 0,
) {

    private var badge: FrameLayout? = null
    private var icon: ImageView? = null

    private val wm: WindowManager?
        get() = ctx.getSystemService(WindowManager::class.java)

    /** 배지 표시/갱신 — [color]=상태 tint. 권한이 없으면 조용히 무시(허용 시 다음 갱신부터 표시). */
    fun update(color: Int, contentDesc: String? = null) {
        if (!Settings.canDrawOverlays(ctx)) return
        runCatching {
            if (badge == null) attach()
            icon?.imageTintList = ColorStateList.valueOf(color)
            icon?.contentDescription = contentDesc
        }
    }

    fun hide() {
        badge?.let { runCatching { wm?.removeView(it) } }
        badge = null; icon = null
    }

    private fun attach() {
        val d = ctx.resources.displayMetrics.density
        fun dp(v: Int) = (v * d).toInt()

        val iconView = ImageView(ctx).apply {
            setImageResource(iconRes)
            layoutParams = FrameLayout.LayoutParams(dp(15), dp(15), Gravity.CENTER)
        }
        val circle = FrameLayout(ctx).apply {
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(0xCC202124.toInt())          // 반투명 짙은 회색 원
            }
            addView(iconView)
        }

        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        } else {
            @Suppress("DEPRECATION") WindowManager.LayoutParams.TYPE_PHONE
        }
        val params = WindowManager.LayoutParams(
            dp(24), dp(24), type,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
            x = dp(xOffsetDp)                         // 중앙 기준 좌우 오프셋(앱별 자리)
            y = 0                                     // 상태바 바로 아래
        }
        wm?.addView(circle, params)
        badge = circle; icon = iconView
    }
}
