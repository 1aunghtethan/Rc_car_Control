package com.vega.rccar

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.widget.Button
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID

/**
 * RC Car controller — Bluetooth CLASSIC (SPP), which is what an HC-05 speaks.
 *
 * WHY THIS APP EXISTS: a web page cannot do this. Web Bluetooth is BLE-only and
 * cannot read the phone's saved/paired device list. Android can do both, so this
 * app connects straight to the HC-05 you already paired in Settings — no scanning.
 *
 * It speaks the same text line protocol as the HTML page:
 *   F B L R S · V:<0-255> · G:<0-180> · !R:1 !P:1 !O:1 !H:1 !T · ?D
 */
class MainActivity : AppCompatActivity() {

    companion object {
        // Standard Serial Port Profile UUID used by HC-05 / HC-06.
        private val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")
        private const val REQ_BT = 1001
        private const val PREFS = "rccar"
        private const val KEY_LAST_MAC = "last_mac"
    }

    private var adapter: BluetoothAdapter? = null
    private var socket: BluetoothSocket? = null
    private var out: OutputStream? = null
    private var inStream: InputStream? = null
    @Volatile private var connected = false
    private val ui = Handler(Looper.getMainLooper())

    private lateinit var prefs: SharedPreferences
    private var lastMac: String? = null

    private lateinit var statusTxt: TextView
    private lateinit var hintTxt: TextView
    private lateinit var connectBtn: Button
    private lateinit var distTxt: TextView
    private lateinit var speedTxt: TextView
    private lateinit var servoSeek: SeekBar
    private lateinit var servoTxt: TextView
    private lateinit var recBtn: Button
    private lateinit var playBtn: Button
    private lateinit var testOutTxt: TextView

    private var recording = false
    private var playing = false
    private var obstacle = true

    /** True while a manual "Read distance" test is waiting for its reply. */
    private var distTestPending = false
    private val testTimeouts = mutableListOf<Runnable>()

    /** Speed gear, 1-4. Range 170..255 so even gear 1 moves the car. */
    private var gear = 2
    private val gearPwm = intArrayOf(0, 170, 200, 225, 255)   // index by gear

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        adapter = BluetoothAdapter.getDefaultAdapter()
        prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        lastMac = prefs.getString(KEY_LAST_MAC, null)

        statusTxt = findViewById(R.id.statusTxt)
        hintTxt = findViewById(R.id.hintTxt)
        connectBtn = findViewById(R.id.connectBtn)
        distTxt = findViewById(R.id.distTxt)
        speedTxt = findViewById(R.id.speedTxt)
        servoSeek = findViewById(R.id.servoSeek)
        servoTxt = findViewById(R.id.servoTxt)
        recBtn = findViewById(R.id.recBtn)
        playBtn = findViewById(R.id.playBtn)
        testOutTxt = findViewById(R.id.testOutTxt)

        connectBtn.setOnClickListener {
            if (connected) { disconnect(); return@setOnClickListener }
            if (!hasPermission()) { requestBtPermission(); return@setOnClickListener }
            // One tap: reconnect to the saved device. Only fall back to the
            // picker if nothing is remembered yet.
            val target = savedDevice()
            if (target != null) connect(target) else pickDevice()
        }
        findViewById<Button>(R.id.chooseBtn).setOnClickListener { pickDevice() }

        holdButton(R.id.btnF, "F")
        holdButton(R.id.btnB, "B")
        holdButton(R.id.btnL, "L")
        holdButton(R.id.btnR, "R")
        holdButton(R.id.btnS, "S")

        /* Speed: 1-4 gear buttons replace the old slider. Sent as explicit
           V:<pwm> -- a bare digit would hit the firmware's n*28 path and make
           gear 4 only 112, nowhere near the 255 we want. */
        val gearIds = intArrayOf(R.id.g1, R.id.g2, R.id.g3, R.id.g4)
        gearIds.forEachIndexed { i, id ->
            findViewById<Button>(id).setOnClickListener { setGear(i + 1) }
        }
        findViewById<Button>(R.id.speedMinusBtn).setOnClickListener { setGear(gear - 1) }
        findViewById<Button>(R.id.speedPlusBtn).setOnClickListener { setGear(gear + 1) }

        servoSeek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) {
                servoTxt.text = "$p°"
                if (fromUser) send("G:$p")
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {}
        })

        findViewById<Button>(R.id.servoLeftBtn).setOnClickListener { setServo(0) }
        findViewById<Button>(R.id.servoCenterBtn).setOnClickListener { setServo(90) }
        findViewById<Button>(R.id.servoRightBtn).setOnClickListener { setServo(180) }
        findViewById<Button>(R.id.servoSweepBtn).setOnClickListener { send("!T") }

        /* ---- hardware self-test ---- */
        findViewById<Button>(R.id.distTestBtn).setOnClickListener { runDistanceTest() }
        findViewById<Button>(R.id.servoTestBtn).setOnClickListener { runServoTest() }

        recBtn.setOnClickListener {
            recording = !recording
            if (recording) playing = false
            send("!R:" + if (recording) 1 else 0)
            recBtn.alpha = if (recording) 1f else 0.6f
            playBtn.alpha = 0.6f
        }
        playBtn.setOnClickListener {
            playing = !playing
            if (playing) recording = false
            send("!P:" + if (playing) 1 else 0)
            playBtn.alpha = if (playing) 1f else 0.6f
            recBtn.alpha = 0.6f
        }
        findViewById<Button>(R.id.obsBtn).setOnClickListener { v ->
            obstacle = !obstacle
            send("!O:" + if (obstacle) 1 else 0)
            (v as Button).alpha = if (obstacle) 1f else 0.6f
        }
        findViewById<Button>(R.id.hornBtn).setOnClickListener {
            send("!H:1")
            ui.postDelayed({ send("!H:0") }, 400)
        }
        findViewById<Button>(R.id.estopBtn).setOnClickListener {
            recording = false; playing = false
            recBtn.alpha = 0.6f; playBtn.alpha = 0.6f
            send("S"); send("!R:0"); send("!P:0")
        }

        if (!hasPermission()) requestBtPermission()
        setGear(gear)          // paint the initial gear highlight (7)
        refreshLabels()
    }

    /* ------------------------------------------------------------------ */
    /*  Saved / paired devices                                            */
    /* ------------------------------------------------------------------ */

    /** The remembered device, if it is still paired with this phone. */
    @SuppressLint("MissingPermission")
    private fun savedDevice(): BluetoothDevice? {
        if (!hasPermission()) return null
        val mac = lastMac ?: return null
        return try {
            adapter?.bondedDevices?.firstOrNull { it.address == mac }
        } catch (_: Exception) { null }
    }

    /**
     * On first run, if the phone has a saved device whose name looks like an
     * HC-05, remember it so Connect is a single tap from then on.
     */
    @SuppressLint("MissingPermission")
    private fun adoptHc05IfUnset() {
        if (lastMac != null || !hasPermission()) return
        try {
            val hc = adapter?.bondedDevices?.firstOrNull {
                val n = it.name ?: ""
                n.contains("HC-05", true) || n.contains("HC-06", true)
            }
            if (hc != null) {
                lastMac = hc.address
                prefs.edit().putString(KEY_LAST_MAC, hc.address).apply()
            }
        } catch (_: Exception) { }
    }

    @SuppressLint("MissingPermission")
    private fun refreshLabels() {
        adoptHc05IfUnset()
        val d = savedDevice()
        if (d != null) {
            connectBtn.text = "Connect to ${d.name ?: "saved device"}"
            hintTxt.text = "Saved device found — no scanning needed. " +
                           "\"Devices\" picks a different one."
        } else {
            connectBtn.text = "Connect to car"
            hintTxt.text = "Pair the HC-05 in Android Bluetooth settings first, " +
                           "then tap \"Devices\" to pick it."
        }
        if (!connected) statusTxt.text = "Disconnected"
    }

    private fun requestBtPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            ActivityCompat.requestPermissions(
                this,
                arrayOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN),
                REQ_BT
            )
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_BT) {
            if (hasPermission()) refreshLabels()
            else toast("Bluetooth permission is required to reach the car.")
        }
    }

    /* ------------------------------------------------------------------ */
    /*  UI helpers                                                        */
    /* ------------------------------------------------------------------ */

    private fun setServo(v: Int) {
        servoSeek.progress = v
        servoTxt.text = "$v°"
        send("G:$v")
    }

    /* ------------------------------------------------------------------ */
    /*  Speed gears (1-9)                                                 */
    /* ------------------------------------------------------------------ */

    private val gearButtonIds = intArrayOf(R.id.g1, R.id.g2, R.id.g3, R.id.g4)

    /** Clamp to 1-4, highlight the active gear, and send explicit V:<pwm>. */
    private fun setGear(g: Int) {
        val n = g.coerceIn(1, 4)
        gear = n
        val pwm = gearPwm[n]
        speedTxt.text = "Gear $n  ($pwm)"
        gearButtonIds.forEachIndexed { i, id ->
            val b = findViewById<Button>(id)
            val on = (i + 1) == n
            b.backgroundTintList = android.content.res.ColorStateList.valueOf(
                getColor(if (on) R.color.accent else R.color.panel)
            )
            b.setTextColor(getColor(if (on) android.R.color.black else R.color.txt))
        }
        send("V:$pwm")          // explicit PWM, not a bare digit
    }

    /** A D-pad button: press -> drive, release -> stop (except STOP itself). */
    @SuppressLint("ClickableViewAccessibility")
    private fun holdButton(id: Int, cmd: String) {
        findViewById<Button>(id).setOnTouchListener { v, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { v.alpha = 0.6f; send(cmd); true }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    v.alpha = 1f
                    if (cmd != "S") send("S")
                    true
                }
                else -> false
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /*  Bluetooth                                                         */
    /* ------------------------------------------------------------------ */

    private fun hasPermission(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
        else true

    @SuppressLint("MissingPermission")
    private fun pickDevice() {
        val a = adapter
        if (a == null) { toast("This phone has no Bluetooth."); return }
        if (!a.isEnabled) { toast("Turn Bluetooth ON first."); return }
        if (!hasPermission()) { requestBtPermission(); return }

        // These are the phone's SAVED (paired) devices — no scanning involved.
        val bonded = try { a.bondedDevices?.toList() ?: emptyList() } catch (_: Exception) { emptyList() }
        if (bonded.isEmpty()) {
            toast("No saved devices. Pair the HC-05 in Bluetooth settings first.")
            return
        }

        val labels = bonded.map {
            val star = if (it.address == lastMac) "★ " else ""
            "$star${it.name ?: "Unknown"}\n${it.address}"
        }.toTypedArray()

        AlertDialog.Builder(this)
            .setTitle("Saved devices")
            .setItems(labels) { _, which -> connect(bonded[which]) }
            .setNegativeButton("Cancel", null)
            .show()
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        statusTxt.text = "Connecting to ${device.name ?: device.address} …"
        Thread {
            try {
                adapter?.cancelDiscovery()
                val s = device.createRfcommSocketToServiceRecord(SPP_UUID)
                s.connect()
                socket = s
                out = s.outputStream
                inStream = s.inputStream
                connected = true
                // Remember it so the next launch is one tap.
                lastMac = device.address
                prefs.edit().putString(KEY_LAST_MAC, device.address).apply()
                ui.post {
                    statusTxt.text = "Connected: ${device.name ?: device.address}"
                    connectBtn.text = "Disconnect"
                    hintTxt.text = "Saved — next time just tap Connect."
                }
                readLoop()
            } catch (e: Exception) {
                ui.post { statusTxt.text = "Connect failed: ${e.message}" }
                disconnect()
            }
        }.start()
    }

    private fun readLoop() {
        val input = inStream ?: return
        val buf = ByteArray(256)
        val sb = StringBuilder()
        try {
            while (connected) {
                val n = input.read(buf)
                if (n <= 0) continue
                sb.append(String(buf, 0, n))
                while (true) {
                    val idx = sb.indexOf("\n")
                    if (idx < 0) break
                    val line = sb.substring(0, idx).trim()
                    sb.delete(0, idx + 1)
                    if (line.isNotEmpty()) ui.post { handleLine(line) }
                }
            }
        } catch (_: Exception) {
            // socket closed
        }
    }

    private fun handleLine(line: String) {
        if (line.startsWith("DIST:")) {
            val cm = line.substring(5).trim().toIntOrNull() ?: return
            distTxt.text = if (cm >= 999) "Distance: -- cm" else "Distance: $cm cm"
            if (distTestPending) showDistanceTestResult(cm)
        } else if (line.startsWith("EVT:")) {
            if (line.contains("OBSTACLE")) distTxt.text = "⚠ Obstacle!"
        }
    }

    /* ------------------------------------------------------------------ */
    /*  Hardware self-test                                                */
    /* ------------------------------------------------------------------ */

    private fun testOut(msg: String, colorRes: Int) {
        testOutTxt.text = msg
        testOutTxt.setTextColor(getColor(colorRes))
    }

    /** Cancel any outstanding test timeout so a late reply cannot overwrite
     *  the result of a newer test. */
    private fun clearTestTimeouts() {
        testTimeouts.forEach { ui.removeCallbacks(it) }
        testTimeouts.clear()
    }

    private fun runDistanceTest() {
        if (!connected) { testOut("Connect to the car first.", R.color.muted); return }
        clearTestTimeouts()
        // No background ?D poll runs in this app, so a one-shot ?D reply is
        // unambiguously ours -- there is no poll reply to confuse it with.
        // (The web app DOES poll, and pauses its poll during a test for the
        //  same reason. Do not add a poll here without adding that guard.)
        distTestPending = true
        testOut("Reading...", R.color.muted)

        val btn = findViewById<Button>(R.id.distTestBtn)
        btn.isEnabled = false
        send("?D")                                   // one-shot distance read

        // If nothing comes back, the sensor or the link is at fault.
        val loss = Runnable {
            if (distTestPending) {
                distTestPending = false
                testOut("No reply from the car. Is it connected and powered?", R.color.danger)
            }
            btn.isEnabled = true
        }
        testTimeouts.add(loss)
        ui.postDelayed(loss, 2500)
    }

    private fun showDistanceTestResult(cm: Int) {
        distTestPending = false
        clearTestTimeouts()
        findViewById<Button>(R.id.distTestBtn).isEnabled = true
        if (cm >= 999) {
            testOut("No echo detected. Check the HC-SR04 wiring on TRIG 11 / ECHO 12.", R.color.danger)
        } else {
            testOut("Ultrasonic OK — obstacle is $cm cm away.", R.color.ok)
        }
    }

    private fun runServoTest() {
        if (!connected) { testOut("Connect to the car first.", R.color.muted); return }
        clearTestTimeouts()
        distTestPending = false
        testOut("Sweeping 0° → 90° → 180° → 90°... watch the servo.", R.color.muted)

        val btn = findViewById<Button>(R.id.servoTestBtn)
        btn.isEnabled = false
        send("!T")                                   // firmware sweep self-test

        // The firmware sweep is blocking (400+400+400+200 ms) and sends no
        // completion event, so we cannot truly detect failure. Report it as
        // sent, and say what to check -- rather than claiming it succeeded.
        val done = Runnable {
            testOut("Sweep sent. If the servo did not move, check its signal wire on pin 3.", R.color.muted)
            btn.isEnabled = true
        }
        testTimeouts.add(done)
        ui.postDelayed(done, 1800)
    }

    private fun send(cmd: String) {
        val o = out ?: return
        try { o.write((cmd + "\n").toByteArray()) } catch (_: Exception) { }
    }

    private fun disconnect() {
        connected = false
        try { socket?.close() } catch (_: Exception) {}
        socket = null; out = null; inStream = null
        ui.post {
            // A test started before the drop must not report a result afterwards.
            distTestPending = false
            clearTestTimeouts()
            findViewById<Button>(R.id.distTestBtn).isEnabled = true
            findViewById<Button>(R.id.servoTestBtn).isEnabled = true
            if (testOutTxt.text != "Tap a button to test a sensor.") {
                testOutTxt.text = "Disconnected."
                testOutTxt.setTextColor(getColor(R.color.muted))
            }
            statusTxt.text = "Disconnected"
            refreshLabels()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        disconnect()
    }

    private fun toast(m: String) = Toast.makeText(this, m, Toast.LENGTH_SHORT).show()
}
