// Copyright 2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#if defined(WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <Windows.h>
#endif

#include "rendererPlugin.h"

#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include "config.h"
#include "renderDelegate.h"

#ifdef WIN32
// TF_REGISTER does not seem to work correctly for external plugins on Windows.
// To get around this, we manually register the plugin with USD on library load
BOOL APIENTRY
DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
        pxr::HdRendererPluginRegistry::Define<HdOSPRayRendererPlugin>();
    } break;
    default:
        break;
    }
    return TRUE;
}
#endif
PXR_NAMESPACE_OPEN_SCOPE
// Register OSPRay plugin with USD
TF_REGISTRY_FUNCTION(TfType)
{
    HdRendererPluginRegistry::Define<HdOSPRayRendererPlugin>();
}
PXR_NAMESPACE_CLOSE_SCOPE

HdRenderDelegate*
HdOSPRayRendererPlugin::CreateRenderDelegate()
{
    // Check supported pxr version
#if PXR_MAJOR_VERSION != 0 || PXR_MINOR_VERSION < 20
    error This version of HdOSPRay is configured to built against USD
           v0 .20.x to v0 .23.x
#endif

    int ac = 1;
    std::string initArgs = HdOSPRayConfig::GetInstance().initArgs;
    std::stringstream ss(initArgs);
    std::string arg;
    std::vector<std::string> args;
    while (ss >> arg) {
        args.push_back(arg);
    }
    if (HdOSPRayConfig::GetInstance().device == "gpu") {
        args.push_back("--osp:load-modules=gpu");
        args.push_back("--osp:device=gpu");
    }
    ac = static_cast<int>(args.size() + 1);
    const char** av = new const char*[ac];
    av[0] = "ospray";
    for (int i = 1; i < ac; i++) {
        av[i] = args[i - 1].c_str();
    }
    int init_error = ospInit(&ac, av);
    if (init_error != OSP_NO_ERROR) {
        std::cerr << "FATAL ERROR DURING INITIALIZATION!" << std::endl;
    } else {
        auto device = ospGetCurrentDevice();
        if (device == nullptr) {
            std::cerr << "FATAL ERROR DURING GETTING CURRENT DEVICE!"
                      << std::endl;
        }

        ospDeviceSetStatusCallback(
               device,
               [](void*, const char* msg) {
                   std::cerr << "OSPRAY STATUS: " << msg << std::endl;
               },
               nullptr);
        ospDeviceSetErrorCallback(
               device,
               [](void*, OSPError e, const char* msg) {
                   std::cerr << "OSPRAY ERROR [" << e << "]: " << msg
                             << std::endl;
               },
               nullptr);

        ospDeviceCommit(device);
    }
    if (ospGetCurrentDevice() == nullptr) {
        // bad user arguments, init with empty arguments instead
        ac = 1;
        ospInit(&ac, av);
    }
    delete[] av;

    return new HdOSPRayRenderDelegate();
}

void
HdOSPRayRendererPlugin::DeleteRenderDelegate(HdRenderDelegate* renderDelegate)
{
    delete renderDelegate;
    ospShutdown();
}

bool
#if PXR_VERSION < 2302
HdOSPRayRendererPlugin::IsSupported() const
#else
HdOSPRayRendererPlugin::IsSupported(bool gpuEnabled) const
#endif
{
    return true;
}