package com.brl.telemetry.network

import android.view.View
import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ReactShadowNode
import com.facebook.react.uimanager.ViewManager

/** Registers BrlNetworkModule — manually wired in MainApplication.getPackages(). */
class BrlNetworkPackage : ReactPackage {
    override fun createNativeModules(rc: ReactApplicationContext): List<NativeModule> =
        listOf(BrlNetworkModule(rc))

    override fun createViewManagers(rc: ReactApplicationContext)
            : List<ViewManager<out View, out ReactShadowNode<*>>> = emptyList()
}
