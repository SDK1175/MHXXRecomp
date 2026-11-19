package kr.mhxx.hubadmin

import com.jcraft.jsch.ChannelExec
import com.jcraft.jsch.JSch
import com.jcraft.jsch.Session
import java.io.ByteArrayOutputStream

/**
 * The SSH connection to the hub server's VM: it reads the admin token and forwards a port on this phone's loopback
 * address to the admin view (127.0.0.1:39520 on the server, docs/hub-admin.md).
 */
class Tunnel(
    private val host: String,
    private val port: Int,
    private val user: String,
) {
    class HostKeyChanged(val seen: String) : Exception("server host key changed")

    private var session: Session? = null
    var localPort = 0
        private set
    var token = ""
        private set
    var hostKey = ""
        private set

    val alive: Boolean get() = session?.isConnected == true

    /**
     * [known]: the server's host key fingerprint seen before ("" the first time: trusted and returned in [hostKey]).
     * A different one stops here, before the token is read ([HostKeyChanged]).
     */
    fun open(privateKey: ByteArray, publicKey: String, known: String) {
        close()
        val jsch = JSch()
        jsch.addIdentity("phone", privateKey, publicKey.toByteArray(), null)
        val s = jsch.getSession(user, host, port)
        // (Android signs and verifies these without extra libraries; OpenSSH offers them by default)
        s.setConfig("StrictHostKeyChecking", "no")
        s.setConfig("server_host_key", "ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256")
        s.setConfig("kex", "ecdh-sha2-nistp256,ecdh-sha2-nistp384,diffie-hellman-group14-sha256")
        s.setConfig("PreferredAuthentications", "publickey")
        s.setServerAliveInterval(15_000)
        s.setServerAliveCountMax(3)
        s.timeout = 15_000
        s.connect(15_000)
        val fp = s.hostKey.getFingerPrint(jsch)
        if (known.isNotEmpty() && known != fp) {
            s.disconnect()
            throw HostKeyChanged(fp)
        }
        hostKey = fp
        session = s
        token = exec("cat /var/lib/mhxx-hubserver/admin.token").trim()
        if (token.length < 32) {
            close()
            throw IllegalStateException("관리 토큰을 읽지 못했습니다. 서버의 허브 서버가 최신인지 확인하세요 (설치 명령 다시 실행).")
        }
        localPort = s.setPortForwardingL("127.0.0.1", 0, "127.0.0.1", ADMIN_PORT)
    }

    fun exec(cmd: String): String {
        val s = session ?: throw IllegalStateException("not connected")
        val ch = s.openChannel("exec") as ChannelExec
        ch.setCommand(cmd)
        val out = ByteArrayOutputStream()
        val err = ByteArrayOutputStream()
        ch.outputStream = out
        ch.setErrStream(err)
        ch.connect(10_000)
        val until = System.currentTimeMillis() + 30_000
        while (!ch.isClosed && System.currentTimeMillis() < until) Thread.sleep(50)
        ch.disconnect()
        if (ch.exitStatus != 0) throw IllegalStateException(err.toString().ifBlank { "명령 실패 (${ch.exitStatus})" })
        return out.toString()
    }

    fun close() {
        runCatching { session?.disconnect() }
        session = null
        localPort = 0
    }

    companion object {
        const val ADMIN_PORT = 39520
    }
}
