// Android 접점 — 자격 저장 (docs/design/features/android_dispatch_tablet.md §5, ue_sdk.md §5.1)
//
// **이 층은 프로토콜을 모른다.** 무엇을 저장하는지(refresh token·H(A1)·로그인 id)는 앱이 정하고,
// 여기는 "기기 밖으로 나가지 않는 키로 암호화해 보관"만 한다. Windows 접점의 DPAPI 자리와 같다
// (sdk/windows/dotnet/CimsUe/Platform/CredentialStore.cs).
//
// 키는 **Android Keystore** 에 두고 암호문만 SharedPreferences 에 남긴다. androidx.security-crypto 를
// 쓰지 않는 이유는 의존을 늘리지 않기 위해서다 — 필요한 것은 AES/GCM 한 벌뿐이다.
package com.cims.ue.sdk.platform

import android.content.Context
import android.content.SharedPreferences
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * 기기 종속 암호화 저장소. 하드웨어 키 저장소가 있으면 키가 거기 머무르고 앱은 핸들만 갖는다.
 *
 * 복호화 실패(기기 초기화·키 무효화·백업 복원)는 **예외가 아니라 null** 로 돌려준다 — 그 경우
 * 앱은 재로그인을 요구하면 된다. 저장 실패도 false 로만 알린다(자격 저장이 앱 기동을 막지 않는다).
 */
class SecureStore(context: Context, private val namespace: String = DEFAULT_NS) {

    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences("cimsue-secure-$namespace", Context.MODE_PRIVATE)

    /** 값 저장. 성공하면 true. */
    fun put(key: String, value: String): Boolean = runCatching {
        val cipher = Cipher.getInstance(TRANSFORM).apply { init(Cipher.ENCRYPT_MODE, secretKey()) }
        val ct = cipher.doFinal(value.toByteArray(Charsets.UTF_8))
        // iv 와 암호문을 한 문자열에 담는다 — GCM iv 는 비밀이 아니고 재사용만 막으면 된다.
        val packed = Base64.encodeToString(cipher.iv, Base64.NO_WRAP) + ":" +
                     Base64.encodeToString(ct, Base64.NO_WRAP)
        prefs.edit().putString(key, packed).commit()
    }.getOrDefault(false)

    /** 값 조회. 없거나 복호화할 수 없으면 null(앱은 재로그인으로 처리한다). */
    fun get(key: String): String? = runCatching {
        val packed = prefs.getString(key, null) ?: return null
        val (ivB64, ctB64) = packed.split(":", limit = 2).let {
            if (it.size != 2) return null else it[0] to it[1]
        }
        val iv = Base64.decode(ivB64, Base64.NO_WRAP)
        val ct = Base64.decode(ctB64, Base64.NO_WRAP)
        val cipher = Cipher.getInstance(TRANSFORM).apply {
            init(Cipher.DECRYPT_MODE, secretKey(), GCMParameterSpec(TAG_BITS, iv))
        }
        String(cipher.doFinal(ct), Charsets.UTF_8)
    }.getOrNull()

    fun remove(key: String) { runCatching { prefs.edit().remove(key).commit() } }

    /** 저장소 비우기 — 로그아웃. 키 자체는 남겨 다음 로그인이 재사용한다. */
    fun clear() { runCatching { prefs.edit().clear().commit() } }

    fun contains(key: String): Boolean = prefs.contains(key)

    private fun secretKey(): SecretKey {
        val ks = KeyStore.getInstance(KEYSTORE).apply { load(null) }
        val alias = "$ALIAS_PREFIX$namespace"
        (ks.getEntry(alias, null) as? KeyStore.SecretKeyEntry)?.let { return it.secretKey }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
        gen.init(
            KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                // 화면 잠금과 묶지 않는다 — 부팅 뒤 무인 재등록이 필요하다(§5 BootRegister).
                .setUserAuthenticationRequired(false)
                .build())
        return gen.generateKey()
    }

    companion object {
        private const val KEYSTORE = "AndroidKeyStore"
        private const val TRANSFORM = "AES/GCM/NoPadding"
        private const val ALIAS_PREFIX = "cimsue-"
        private const val TAG_BITS = 128
        private const val DEFAULT_NS = "default"

        /** 앱이 쓰는 표준 키 이름 — 접점층은 의미를 모르고 이름만 제공한다. */
        const val KEY_REFRESH_TOKEN = "refresh_token"
        const val KEY_LOGIN_ID = "login_id"
        const val KEY_CSC_HOST = "csc_host"
    }
}
