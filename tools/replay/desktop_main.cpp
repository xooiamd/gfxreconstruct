/*
** Copyright (c) 2018-2020 Valve Corporation
** Copyright (c) 2018-2020 LunarG, Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include "replay_settings.h"

#include "application/application.h"
#include "decode/file_processor.h"
#include "decode/vulkan_replay_options.h"
#include "decode/vulkan_resource_tracking_consumer.h"
#include "generated/generated_vulkan_decoder.h"
#include "generated/generated_vulkan_replay_consumer.h"
#include "util/argument_parser.h"
#include "util/date_time.h"
#include "util/logging.h"

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(WIN32)
#if defined(VK_USE_PLATFORM_WIN32_KHR)
#include "application/win32_application.h"
#include "application/win32_window.h"
#endif
#else
#if defined(VK_USE_PLATFORM_XCB_KHR)
#include "application/xcb_application.h"
#include "application/xcb_window.h"
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
#include "application/wayland_application.h"
#include "application/wayland_window.h"
#endif
#endif

const char kLayerEnvVar[] = "VK_INSTANCE_LAYERS";

void run_first_pass_replay_portability(const gfxrecon::decode::ReplayOptions              replay_options,
                                       gfxrecon::decode::VulkanDecoder                    decoder,
                                       gfxrecon::util::ArgumentParser                     arg_parser,
                                       gfxrecon::decode::VulkanResourceTrackingConsumer** resource_tracking_consumer,
                                       std::string                                        filename)
{
    if (replay_options.enable_multipass_replay_portability == true)
    {
        // enable first pass of replay to generate resource tracking information
        GFXRECON_WRITE_CONSOLE("First pass of replay resource tracking for memory portability. This may "
                               "take some time. Please wait...");
        gfxrecon::decode::FileProcessor file_processor_resource_tracking;
        *resource_tracking_consumer = new gfxrecon::decode::VulkanResourceTrackingConsumer(replay_options);

        if (file_processor_resource_tracking.Initialize(filename))
        {
            decoder.AddConsumer(*resource_tracking_consumer);

            file_processor_resource_tracking.AddDecoder(&decoder);
            file_processor_resource_tracking.ProcessAllFrames();

            file_processor_resource_tracking.RemoveDecoder(&decoder);
            decoder.RemoveConsumer(*resource_tracking_consumer);
        }

        // sort the bound resources according to the binding offsets
        (*resource_tracking_consumer)->SortMemoriesBoundResourcesByOffset();

        // calculate the replay binding offset of the bound resources and replay memory allocation size
        (*resource_tracking_consumer)->CalculateReplayBindingOffsetAndMemoryAllocationSize();

        GFXRECON_WRITE_CONSOLE("First pass of replay resource tracking done.");
    }
}

int main(int argc, const char** argv)
{
    int return_code = 0;

    gfxrecon::util::Log::Init();

    gfxrecon::util::ArgumentParser arg_parser(argc, argv, kOptions, kArguments);

    if (CheckOptionPrintVersion(argv[0], arg_parser) || CheckOptionPrintUsage(argv[0], arg_parser))
    {
        gfxrecon::util::Log::Release();
        exit(0);
    }
    else if (arg_parser.IsInvalid() || (arg_parser.GetPositionalArgumentsCount() != 1))
    {
        PrintUsage(argv[0]);
        gfxrecon::util::Log::Release();
        exit(-1);
    }
    else
    {
        ProcessDisableDebugPopup(arg_parser);
    }

    try
    {
        const std::vector<std::string>& positional_arguments = arg_parser.GetPositionalArguments();
        std::string                     filename             = positional_arguments[0];

        gfxrecon::decode::FileProcessor                     file_processor;
        std::unique_ptr<gfxrecon::application::Application> application;
        std::unique_ptr<gfxrecon::decode::WindowFactory>    window_factory;

        if (!file_processor.Initialize(filename))
        {
            return_code = -1;
        }
        else
        {
            auto wsi_platform = GetWsiPlatform(arg_parser);

            // Setup platform specific application and window factory.
#if defined(WIN32)
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            if (wsi_platform == WsiPlatform::kWin32 || (wsi_platform == WsiPlatform::kAuto && !application))
            {
                auto win32_application = std::make_unique<gfxrecon::application::Win32Application>(kApplicationName);
                if (win32_application->Initialize(&file_processor))
                {
                    window_factory =
                        std::make_unique<gfxrecon::application::Win32WindowFactory>(win32_application.get());
                    application = std::move(win32_application);
                }
            }
#endif
#else
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
            if (wsi_platform == WsiPlatform::kWayland || (wsi_platform == WsiPlatform::kAuto && !application))
            {
                auto wayland_application =
                    std::make_unique<gfxrecon::application::WaylandApplication>(kApplicationName);
                if (wayland_application->Initialize(&file_processor))
                {
                    window_factory =
                        std::make_unique<gfxrecon::application::WaylandWindowFactory>(wayland_application.get());
                    application = std::move(wayland_application);
                }
            }
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
            if (wsi_platform == WsiPlatform::kXcb || (wsi_platform == WsiPlatform::kAuto && !application))
            {
                auto xcb_application = std::make_unique<gfxrecon::application::XcbApplication>(kApplicationName);
                if (xcb_application->Initialize(&file_processor))
                {
                    window_factory = std::make_unique<gfxrecon::application::XcbWindowFactory>(xcb_application.get());
                    application    = std::move(xcb_application);
                }
            }
#endif
#endif

            if (!window_factory || !application)
            {
                GFXRECON_WRITE_CONSOLE(
                    "Failed to initialize platform specific window system management.\nEnsure that the appropriate "
                    "Vulkan platform extensions have been enabled.");
                return_code = -1;
            }
            else
            {
                gfxrecon::decode::VulkanDecoder decoder;

                // get user replay option
                const gfxrecon::decode::ReplayOptions replay_options = GetReplayOptions(arg_parser);

                // -m <remap or rebind> and --empr usage should be mutually exclusive, check for the user replay option
                // and stop replay if both are enabled at the same time.
                if ((replay_options.create_resource_allocator != CreateDefaultAllocator) &&
                    (replay_options.enable_multipass_replay_portability == true))
                {
                    GFXRECON_LOG_FATAL(
                        "Multipass (2 pass) replay argument \'--emrp\' cannot be used with single pass memory "
                        "translation argument \'-m\'. Please choose either one of the argument for replay.");
                    return_code = -1;
                }
                else
                {
                    gfxrecon::decode::VulkanResourceTrackingConsumer* resource_tracking_consumer = nullptr;

                    // run first pass of resource tracking in replay for memory portability if enabled by user
                    run_first_pass_replay_portability(
                        replay_options, decoder, arg_parser, &resource_tracking_consumer, filename);

                    // replay trace
                    gfxrecon::decode::VulkanReplayConsumer replay_consumer(
                        window_factory.get(), resource_tracking_consumer, replay_options);

                    replay_consumer.SetFatalErrorHandler(
                        [](const char* message) { throw std::runtime_error(message); });

                    decoder.AddConsumer(&replay_consumer);
                    file_processor.AddDecoder(&decoder);
                    application->SetPauseFrame(GetPauseFrame(arg_parser));

                    // Warn if the capture layer is active.
                    CheckActiveLayers(kLayerEnvVar);

                    // Grab the start frame/time information for the FPS result.
                    uint32_t start_frame = 1;
                    int64_t  start_time  = gfxrecon::util::datetime::GetTimestamp();

                    application->Run();

                    if ((file_processor.GetCurrentFrameNumber() > 0) &&
                        (file_processor.GetErrorState() == gfxrecon::decode::FileProcessor::kErrorNone))
                    {
                        // Grab the end frame/time information and calculate FPS.
                        int64_t end_time      = gfxrecon::util::datetime::GetTimestamp();
                        double  diff_time_sec = gfxrecon::util::datetime::ConvertTimestampToSeconds(
                            gfxrecon::util::datetime::DiffTimestamps(start_time, end_time));
                        uint32_t end_frame    = file_processor.GetCurrentFrameNumber();
                        uint32_t total_frames = (end_frame - start_frame) + 1;
                        double   fps          = static_cast<double>(total_frames) / diff_time_sec;
                        GFXRECON_WRITE_CONSOLE("%f fps, %f seconds, %u frame%s, 1 loop, framerange %u-%u",
                                               fps,
                                               diff_time_sec,
                                               total_frames,
                                               total_frames > 1 ? "s" : "",
                                               start_frame,
                                               end_frame);
                    }
                    else if (file_processor.GetErrorState() != gfxrecon::decode::FileProcessor::kErrorNone)
                    {
                        GFXRECON_WRITE_CONSOLE("A failure has occurred during replay");
                        return_code = -1;
                    }
                    else
                    {
                        GFXRECON_WRITE_CONSOLE("File did not contain any frames");
                    }
                }
            }
        }
    }
    catch (std::runtime_error error)
    {
        GFXRECON_WRITE_CONSOLE("Replay has encountered a fatal error and cannot continue: %s", error.what());
        return_code = -1;
    }
    catch (...)
    {
        GFXRECON_WRITE_CONSOLE("Replay failed due to an unhandled exception");
        return_code = -1;
    }

    gfxrecon::util::Log::Release();

    return return_code;
}
