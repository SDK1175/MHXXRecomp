package kr.mhxx.hubadmin

import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.app.KeyguardManager
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.hardware.biometrics.BiometricManager.Authenticators
import android.hardware.biometrics.BiometricPrompt
import android.net.Uri
import android.os.Bundle
import android.os.CancellationSignal
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.util.concurrent.Executors

/**
 * The hub server's admin app for phones (docs/hub-admin.md, step 5): this phone's SSH key, a tunnel to the server,
 * and the admin page in a WebView. Nothing here talks to anything but the server the user typed in.
 */
class MainActivity : Activity() {
    private val io = Executors.newSingleThreadExecutor()
    private val prefs by lazy { getSharedPreferences("hubadmin", MODE_PRIVATE) }
    private val keys by lazy { Keys(this) }
    private var tunnel: Tunnel? = null
    private var prv: ByteArray? = null // the private key, opened once per run (wiped in onDestroy)
    private var web: WebView? = null
    private var statusDot: View? = null
    private var statusText: TextView? = null
    private var connecting = false

    private val host get() = prefs.getString("host", DEFAULT_HOST)!!
    private val port get() = prefs.getInt("port", 22)
    private val user get() = prefs.getString("user", "root")!!

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        when {
            !keys.exists -> showSetup()
            prefs.getBoolean("registered", false) -> connect()
            else -> showServer()
        }
    }

    override fun onResume() {
        super.onResume()
        // back from the background: the phone may have dropped the connection
        if (web != null && tunnel?.alive != true && !connecting && prv != null) reconnect(quiet = true)
    }

    override fun onDestroy() {
        io.execute { tunnel?.close() }
        prv?.fill(0)
        prv = null
        web?.destroy()
        super.onDestroy()
    }

    @Deprecated("Activity back")
    override fun onBackPressed() {
        val w = web
        if (w == null) {
            @Suppress("DEPRECATION") super.onBackPressed()
            return
        }
        // the page closes an open room first (phones), else the app goes to the background
        w.evaluateJavascript("window.adminBack ? adminBack() : false") { r ->
            if (r != "true") moveTaskToBack(true)
        }
    }

    // ---- screens

    private fun dp(v: Int) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics).toInt()
    private fun color(id: Int) = getColor(id)

    private fun page(build: LinearLayout.() -> Unit) {
        web?.destroy(); web = null
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(22), dp(28), dp(22), dp(28))
            build()
        }
        setContentView(ScrollView(this).apply { addView(col); isFillViewport = true; setBackgroundColor(color(R.color.bg)) })
    }

    private fun LinearLayout.title(s: String) = addView(TextView(context).apply {
        text = s; textSize = 22f; setTypeface(typeface, Typeface.BOLD); setTextColor(color(R.color.text))
    })

    private fun LinearLayout.body(s: String, top: Int = 10) = addView(TextView(context).apply {
        text = s; textSize = 14f; setTextColor(color(R.color.muted)); setLineSpacing(0f, 1.25f)
    }, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = dp(top) })

    private fun LinearLayout.box(s: String): TextView = TextView(context).apply {
        text = s; textSize = 12f; typeface = Typeface.MONOSPACE; setTextColor(color(R.color.text)); setTextIsSelectable(true)
        setPadding(dp(12), dp(10), dp(12), dp(10))
        background = GradientDrawable().apply { cornerRadius = dp(8).toFloat(); setColor(color(R.color.panel)); setStroke(dp(1), color(R.color.line)) }
    }.also { addView(it, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = dp(8) }) }

    private fun LinearLayout.button(s: String, primary: Boolean = true, top: Int = 14, on: () -> Unit): Button =
        Button(context).apply {
            text = s; isAllCaps = false; textSize = 15f
            setTextColor(if (primary) 0xFFFFFFFF.toInt() else color(R.color.text))
            background = GradientDrawable().apply {
                cornerRadius = dp(10).toFloat()
                setColor(if (primary) color(R.color.accent) else color(R.color.panel))
                if (!primary) setStroke(dp(1), color(R.color.line))
            }
            setOnClickListener { on() }
        }.also { addView(it, LinearLayout.LayoutParams(MATCH_PARENT, dp(48)).apply { topMargin = dp(top) }) }

    private fun copy(label: String, s: String) {
        (getSystemService(CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText(label, s))
        Toast.makeText(this, "복사했습니다", Toast.LENGTH_SHORT).show()
    }

    private fun share(s: String) = startActivity(Intent.createChooser(
        Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, s), "공개 열쇠 보내기"))

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_LONG).show()

    /** First run: make this phone's key, behind the screen lock. */
    private fun showSetup() = page {
        title("MHXX 허브 관리")
        body("허브 서버를 폰에서 관리하는 앱입니다. 먼저 이 폰 전용 SSH 열쇠를 만듭니다.\n\n" +
            "• 열쇠는 이 폰 안에만 저장되고, 화면 잠금(지문·PIN)을 풀어야 열립니다.\n" +
            "• 만든 뒤 나오는 '공개 열쇠' 한 줄을 PC에서 서버에 등록하면 연결됩니다.\n" +
            "• 폰을 잃어버리면 서버에서 그 한 줄만 지우면 됩니다.")
        button("이 폰의 열쇠 만들기", top = 24) {
            val km = getSystemService(KeyguardManager::class.java)
            if (!km.isDeviceSecure) {
                toast("먼저 폰에 화면 잠금(PIN, 패턴, 지문 등)을 설정해 주세요.")
                return@button
            }
            unlock("열쇠 만들기") {
                io.execute {
                    val r = runCatching { keys.create() }
                    runOnUiThread {
                        r.onSuccess { showServer() }.onFailure { toast("열쇠를 만들지 못했습니다: ${it.message}") }
                    }
                }
            }
        }
    }

    /** The server's address, and how to register this phone's key on it. */
    private fun showServer() = page {
        val pub = keys.publicKey
        title("서버 연결")
        body("허브 서버 주소")
        val hostIn = EditText(context).apply {
            setText(host); inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI; isSingleLine = true
            setTextColor(color(R.color.text))
        }
        addView(hostIn)
        body("이 폰의 공개 열쇠 (보여도 괜찮은 정보입니다)", top = 18)
        box(pub)
        val cmd = "echo '$pub' >> ~/.ssh/authorized_keys"
        body("PC의 PowerShell로 서버에 접속한 뒤(ssh -i ...), 아래 한 줄을 실행하면 이 폰이 등록됩니다.", top = 18)
        box(cmd)
        button("등록 명령 복사", primary = false) { copy("등록 명령", cmd) }
        button("PC로 보내기 (카톡 나에게 등)", primary = false, top = 8) { share(cmd) }
        button("연결", top = 22) {
            val h = hostIn.text.toString().trim()
            if (h.isEmpty()) return@button
            if (h != host) prefs.edit().putString("host", h).remove("hostkey").apply()
            connect()
        }
        body("한 번 연결되면 다음부터는 앱을 열 때 화면 잠금만 풀면 바로 관리 화면이 뜹니다.", top = 14)
    }

    private fun showConnecting(msg: String) = page {
        gravity = Gravity.CENTER
        addView(ProgressBar(context), LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { gravity = Gravity.CENTER_HORIZONTAL; topMargin = dp(120) })
        addView(TextView(context).apply { text = msg; textSize = 15f; gravity = Gravity.CENTER; setTextColor(color(R.color.muted)) },
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = dp(16) })
    }

    // ---- connecting

    private fun unlock(what: String, ok: () -> Unit) {
        BiometricPrompt.Builder(this)
            .setTitle("MHXX 허브 관리")
            .setSubtitle(what)
            .setAllowedAuthenticators(Authenticators.BIOMETRIC_STRONG or Authenticators.DEVICE_CREDENTIAL)
            .build()
            .authenticate(CancellationSignal(), mainExecutor, object : BiometricPrompt.AuthenticationCallback() {
                override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) = ok()
                override fun onAuthenticationError(code: Int, msg: CharSequence) {
                    if (web == null && !prefs.getBoolean("registered", false)) return
                    if (web == null) toast("잠금 해제를 취소했습니다. 앱을 다시 열어 주세요.")
                }
            })
    }

    private fun connect() {
        if (prv != null) return reconnect(quiet = false)
        unlock("관리 화면 열기") {
            io.execute {
                val r = runCatching { keys.privateKey() }
                runOnUiThread {
                    r.onSuccess { prv = it; reconnect(quiet = false) }
                        .onFailure { toast("열쇠를 열지 못했습니다: ${it.message}"); showServer() }
                }
            }
        }
    }

    private fun reconnect(quiet: Boolean, trust: String? = null) {
        val key = prv ?: return connect()
        if (connecting) return
        connecting = true
        if (!quiet || web == null) showConnecting("$host 에 연결하는 중…") else setStatus(false, "다시 연결하는 중…")
        val known = trust ?: prefs.getString("hostkey", "")!!
        io.execute {
            val t = Tunnel(host, port, user)
            val r = runCatching { tunnel?.close(); t.open(key, keys.publicKey, known); t }
            runOnUiThread {
                connecting = false
                r.onSuccess {
                    tunnel = it
                    prefs.edit().putString("hostkey", it.hostKey).putBoolean("registered", true).apply()
                    if (web == null || !quiet) showWeb(it) else { loadPage(it); setStatus(true, host) }
                }.onFailure { e ->
                    when (e) {
                        is Tunnel.HostKeyChanged -> hostKeyChanged(e.seen)
                        else -> failed(e)
                    }
                }
            }
        }
    }

    private fun failed(e: Throwable) {
        val msg = e.message ?: e.toString()
        val hint = when {
            msg.contains("Auth fail", true) -> "서버에 이 폰의 공개 열쇠가 등록되지 않았습니다. 등록 명령을 PC에서 실행했는지 확인하세요."
            msg.contains("timeout", true) || msg.contains("UnknownHost", true) -> "서버에 닿지 않습니다. 주소와 인터넷 연결을 확인하세요."
            else -> msg
        }
        if (web != null) {
            setStatus(false, "연결 끊김"); toast(hint); return
        }
        page {
            title("연결하지 못했습니다")
            body(hint)
            button("다시 시도", top = 22) { reconnect(quiet = false) }
            button("서버 주소·열쇠 등록 보기", primary = false, top = 8) { showServer() }
        }
    }

    private fun hostKeyChanged(seen: String) {
        AlertDialog.Builder(this)
            .setTitle("서버 열쇠가 바뀌었습니다")
            .setMessage("전에 연결한 서버와 다른 서버가 응답했습니다.\n\n서버를 새로 만들었거나 운영체제를 다시 설치했다면 괜찮습니다. 그런 적이 없다면 연결하지 마세요.\n\n새 지문: $seen")
            .setNegativeButton("연결 안 함") { _, _ -> failed(Exception("서버 열쇠가 바뀌어 연결하지 않았습니다.")) }
            .setPositiveButton("새 서버 믿기") { _, _ -> reconnect(quiet = false, trust = seen) }
            .setCancelable(false)
            .show()
    }

    // ---- the admin page

    @SuppressLint("SetJavaScriptEnabled")
    private fun showWeb(t: Tunnel) {
        web?.destroy()
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setBackgroundColor(color(R.color.bg)) }
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(16), dp(6), dp(4), dp(6)); setBackgroundColor(color(R.color.panel))
        }
        statusDot = View(this).apply { background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(0xFF4CC685.toInt()) } }
        bar.addView(statusDot, LinearLayout.LayoutParams(dp(8), dp(8)))
        statusText = TextView(this).apply { textSize = 12f; setTextColor(color(R.color.muted)); text = host }
        bar.addView(statusText, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f).apply { leftMargin = dp(8) })
        val more = Button(this).apply {
            text = "⋮"; textSize = 20f; setTextColor(color(R.color.text)); background = null
            setOnClickListener { menu(this) }
        }
        bar.addView(more, LinearLayout.LayoutParams(dp(48), dp(40)))
        root.addView(bar, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        val w = WebView(this).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.allowFileAccess = false
            settings.allowContentAccess = false
            setBackgroundColor(color(R.color.bg))
            webViewClient = object : WebViewClient() {
                // only the tunnel's page; anything else opens outside the app
                override fun shouldOverrideUrlLoading(view: WebView, req: WebResourceRequest): Boolean {
                    if (req.url.host == "127.0.0.1") return false
                    runCatching { startActivity(Intent(Intent.ACTION_VIEW, req.url)) }
                    return true
                }
            }
        }
        web = w
        root.addView(FrameLayout(this).apply { addView(w) }, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        setContentView(root)
        loadPage(t)
    }

    private fun loadPage(t: Tunnel) {
        web?.loadUrl("http://127.0.0.1:${t.localPort}/?token=${Uri.encode(t.token)}&device=phone")
    }

    private fun setStatus(ok: Boolean, s: String) {
        (statusDot?.background as? GradientDrawable)?.setColor(if (ok) 0xFF4CC685.toInt() else color(R.color.bad))
        statusText?.text = s
    }

    private fun menu(anchor: View) {
        PopupMenu(this, anchor).apply {
            menu.add(0, 1, 0, "새로고침")
            menu.add(0, 2, 1, "다시 연결")
            menu.add(0, 3, 2, "허브 서버 재시작")
            menu.add(0, 4, 3, "서버 주소·이 폰 열쇠")
            setOnMenuItemClickListener {
                when (it.itemId) {
                    1 -> web?.reload()
                    2 -> reconnect(quiet = true)
                    3 -> restartHub()
                    4 -> { io.execute { tunnel?.close() }; showServer() }
                }
                true
            }
            show()
        }
    }

    private fun restartHub() {
        AlertDialog.Builder(this)
            .setTitle("허브 서버를 재시작할까요?")
            .setMessage("지금 열린 방과 멀티 중인 사람들의 연결이 모두 끊깁니다. 서버가 이상할 때만 쓰세요.")
            .setNegativeButton("취소", null)
            .setPositiveButton("재시작") { _, _ ->
                setStatus(false, "재시작하는 중…")
                io.execute {
                    val r = runCatching { tunnel?.exec("systemctl restart mhxx-hubserver") }
                    Thread.sleep(1500)
                    runOnUiThread {
                        r.onFailure { toast("재시작하지 못했습니다: ${it.message}") }
                        reconnect(quiet = true)
                    }
                }
            }
            .show()
    }

    companion object {
        const val DEFAULT_HOST = "mhxx-kr.duckdns.org"
    }
}
