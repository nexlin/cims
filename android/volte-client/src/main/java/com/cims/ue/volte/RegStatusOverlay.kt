package com.cims.ue.volte

import android.content.Context
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.TextView

/**
 * 화면 최상단(상태바 바로 아래) 전역 등록상태 배지 — 어떤 앱 위에서든 보인다.
 *
 * 시스템 상태바 자체(wifi/LTE 아이콘 영역)는 시스템 전용이라 서드파티 앱이 그릴 수 없으므로,
 * `TYPE_APPLICATION_OVERLAY`(다른 앱 위에 표시 권한, [Settings.canDrawOverlays]) 로 그 바로 아래에
 * 작은 알약(●+라벨)을 상시 표시한다. 터치는 통과(FLAG_NOT_TOUCHABLE) — 조작 방해 없음.
 *
 * 모든 호출은 main 스레드에서 해야 한다(SipService 코루틴이 보장).
 */
class RegStatusOverlay(private val ctx: Context) {

    private var pill: LinearLayout? = null
    private var dot: View? = null
    private var label: TextView? = null

    private val wm: WindowManager?
        get() = ctx.getSystemService(WindowManager::class.java)

    /** 배지 표시/갱신. 권한이 없으면 조용히 무시(설정에서 허용 시 다음 갱신부터 표시). */
    fun update(color: Int, text: String) {
        if (!Settings.canDrawOverlays(ctx)) return
        runCatching {
            if (pill == null) attach()
            (dot?.background as? GradientDrawable)?.setColor(color)
            label?.text = text
        }
    }

    fun hide() {
        pill?.let { runCatching { wm?.removeView(it) } }
        pill = null; dot = null; label = null
    }

    private fun attach() {
        val d = ctx.resources.displayMetrics.density
        fun dp(v: Int) = (v * d).toInt()

        val dotView = View(ctx).apply {
            background = GradientDrawable().apply { shape = GradientDrawable.OVAL }
            layoutParams = LinearLayout.LayoutParams(dp(8), dp(8)).apply { rightMargin = dp(5) }
        }
        val textView = TextView(ctx).apply {
            textSize = 11f
            setTextColor(Color.WHITE)
        }
        val pillView = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(10), dp(3), dp(10), dp(3))
            background = GradientDrawable().apply {
                cornerRadius = dp(20).toFloat()
                setColor(0xCC202124.toInt())          // 반투명 짙은 회색 알약
            }
            addView(dotView)
            addView(textView)
        }

        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        } else {
            @Suppress("DEPRECATION") WindowManager.LayoutParams.TYPE_PHONE
        }
        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            type,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
            y = 0                                     // 상태바 바로 아래
        }
        wm?.addView(pillView, params)
        pill = pillView; dot = dotView; label = textView
    }
}
