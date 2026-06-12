package com.xros.gltest

import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.view.Surface
import android.view.TextureView
import com.xros.gltest.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private var previewSurface: Surface? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // ---- TextureView: provides a real hardware surface ----
        binding.textureView.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(surfaceTexture: android.graphics.SurfaceTexture, width: Int, height: Int) {
                previewSurface = Surface(surfaceTexture)
                runOnUiThread {
                    binding.btnSurfaceProbe.isEnabled = true
                }
            }

            override fun onSurfaceTextureSizeChanged(surfaceTexture: android.graphics.SurfaceTexture, width: Int, height: Int) {}

            override fun onSurfaceTextureDestroyed(surfaceTexture: android.graphics.SurfaceTexture): Boolean {
                previewSurface = null
                runOnUiThread {
                    binding.btnSurfaceProbe.isEnabled = false
                }
                return true
            }

            override fun onSurfaceTextureUpdated(surfaceTexture: android.graphics.SurfaceTexture) {}
        }

        // ---- Offscreen probe button ----
        binding.btnOffscreenProbe.setOnClickListener {
            binding.probeResult.text = "⏳ Probing with offscreen pbuffer surface..."
            Thread {
                val result = probeExtensions()
                runOnUiThread { binding.probeResult.text = result }
            }.start()
        }

        // ---- Window surface probe button ----
        binding.btnSurfaceProbe.setOnClickListener {
            val surface = previewSurface
            if (surface == null) {
                binding.probeResult.text = "⏳ Waiting for TextureView surface..."
                return@setOnClickListener
            }
            binding.probeResult.text = "⏳ Probing with hardware window surface..."
            Thread {
                val result = probeExtensionsWithSurface(surface)
                runOnUiThread { binding.probeResult.text = result }
            }.start()
        }

        // ---- Auto-run offscreen probe on start ----
        binding.btnOffscreenProbe.performClick()
    }

    /** Offscreen probe — uses EGL pbuffer, no window needed */
    external fun probeExtensions(): String

    /** Window surface probe — uses a real android.view.Surface */
    external fun probeExtensionsWithSurface(surface: Surface): String

    companion object {
        // Used to load the 'gltest' library on application startup.
        init {
            System.loadLibrary("gltest")
        }
    }
}