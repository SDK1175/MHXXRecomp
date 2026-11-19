package kr.mhxx.hubadmin

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import com.jcraft.jsch.JSch
import com.jcraft.jsch.KeyPair
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * This phone's own SSH key (docs/hub-admin.md: one key per device, so a lost phone is one line to delete on the
 * server). The private key is encrypted with an AES key in the Android Keystore that opens only for 30 seconds after
 * the screen lock or a fingerprint; it never leaves the app (no backup).
 *
 * ECDSA P-256: Android's own crypto signs it on every version this app runs on (Ed25519 would need a library).
 */
class Keys(private val ctx: Context) {
    private val dir = File(ctx.filesDir, "ssh").apply { mkdirs() }
    private val enc = File(dir, "key.enc")
    private val pub = File(dir, "key.pub")

    val exists: Boolean get() = enc.exists() && pub.exists()
    val publicKey: String get() = pub.readText().trim()

    /** Needs a fresh unlock (BiometricPrompt) just before: the Keystore key is made to require it. */
    fun create() {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        if (ks.containsAlias(ALIAS)) ks.deleteEntry(ALIAS)
        KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(
                KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setUserAuthenticationRequired(true)
                    .setUserAuthenticationParameters(
                        30, KeyProperties.AUTH_BIOMETRIC_STRONG or KeyProperties.AUTH_DEVICE_CREDENTIAL
                    )
                    .build()
            )
            generateKey()
        }
        val kp = KeyPair.genKeyPair(JSch(), KeyPair.ECDSA, 256)
        val prv = ByteArrayOutputStream().also { kp.writePrivateKey(it) }.toByteArray()
        val pubLine = ByteArrayOutputStream().also { kp.writePublicKey(it, "mhxx-admin-phone") }.toString().trim()
        kp.dispose()
        val c = Cipher.getInstance(TRANSFORM).apply { init(Cipher.ENCRYPT_MODE, secret()) }
        val ct = c.doFinal(prv)
        prv.fill(0)
        enc.writeBytes(byteArrayOf(c.iv.size.toByte()) + c.iv + ct)
        pub.writeText(pubLine + "\n")
    }

    /** The private key (PEM), after a fresh unlock. The caller wipes it when done. */
    fun privateKey(): ByteArray {
        val b = enc.readBytes()
        val n = b[0].toInt()
        val c = Cipher.getInstance(TRANSFORM)
        c.init(Cipher.DECRYPT_MODE, secret(), GCMParameterSpec(128, b, 1, n))
        return c.doFinal(b, 1 + n, b.size - 1 - n)
    }

    fun delete() {
        enc.delete(); pub.delete()
        runCatching { KeyStore.getInstance("AndroidKeyStore").apply { load(null) }.deleteEntry(ALIAS) }
    }

    private fun secret(): SecretKey =
        (KeyStore.getInstance("AndroidKeyStore").apply { load(null) }.getEntry(ALIAS, null) as KeyStore.SecretKeyEntry).secretKey

    companion object {
        private const val ALIAS = "mhxx-hubadmin-ssh"
        private const val TRANSFORM = "AES/GCM/NoPadding"
    }
}
