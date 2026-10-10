# DoWATT

![DoWATT](assets/DoWATT.png)

**v1.0 · Windows x64 · Power and RAM at a glance**

DoWATT is a lightweight, click-through overlay in the top-right corner of your primary screen. It reads physical RAM usage and displays measured or estimated system power.

`≈140 W  |  RAM 21.0/63.8 GiB`

## Use

Run the included **DoWATT.exe**. Right-click the pulse icon in the Windows notification area to:

- Show or hide RAM usage.
- Enable **Start at sign-in**.
- Adjust **Background transparency**, while keeping the text readable.
- View the version/license information or exit.

For startup, place DoWATT in a permanent folder before enabling **Start at sign-in**. You may use `C:\Program Files\DoWATT` (copying there may require administrator approval), or a folder under `%LOCALAPPDATA%\Programs\DoWATT`. DoWATT itself runs without administrator privileges. If you move the executable, enable the option again from its new location.

Settings are saved for your Windows account. To uninstall, disable startup, exit, and delete the application folder.

## Build it yourself

Download [Zig 0.13.0 for Windows x64](https://ziglang.org/download/0.13.0/), then run:

```powershell
.\build.ps1 -ZigPath 'C:\tools\zig\zig.exe'
```

The output is **DoWATT.exe**, file version **1.0.0**. The build uses Zig’s bundled Clang/LLD directly and Windows PowerShell to encode the icon, manifest and version resources. No additional application runtime or driver is bundled.

## Reading the values

RAM is used/usable physical memory in GiB. A verified whole-system input-power meter is used when available; otherwise `≈` means an estimate, rounded to five watts. Missing or incomplete sensor coverage gives `N/A`.

CPU energy counters, optional installed NVIDIA NVML telemetry, RAM inventory and physical disk activity inform the estimate. Generic component-power assumptions and 90% PSU efficiency are used where needed. External displays and actual charging/PSU losses are not fully measured. Use a wall meter for actual outlet consumption.

The application makes no network requests and installs no service or driver. NVML is loaded only from system/vendor installation directories.

## License

Copyright © 2026 Dogukan Sahil. [GNU GPL v3](LICENSE), with the explicit [NVML linking permission](NVML-EXCEPTION.txt). NVIDIA DLLs, drivers, SDK headers and sample code are not distributed. Users must have a properly licensed NVIDIA driver installed to use NVML. NVIDIA components retain their own licenses; the exception does not relicense them.

The pulse icon is original project geometry. Windows DLLs and the installed Segoe UI font are used without redistribution. Zig is a build tool; relevant Zig and MinGW-w64 notices are in [licenses/](licenses/).

Licensing references: [GNU's GPL linking guidance](https://www.gnu.org/licenses/gpl-faq.en.html#GPLIncompatibleLibs), [NVIDIA Driver License](https://www.nvidia.com/en-us/drivers/nvidia-license/).

## Credits

Developed and maintained by Dogukan Sahil. OpenAI Codex assisted with code changes, build troubleshooting, the original icon and documentation. This credit describes AI assistance and does not imply endorsement by OpenAI.

## Disclaimer and third-party rights

DoWATT is provided without warranty, with the warranty disclaimer and limitation of liability governed by sections 15–17 of the GNU GPL v3, to the extent permitted by applicable law. Power estimates are informational and are not certified electrical measurements.

Third-party names and trademarks, including NVIDIA and Microsoft, belong to their respective owners. DoWATT is an independent project and is not affiliated with, sponsored by or endorsed by those companies. NVIDIA binaries, drivers, SDK headers and sample code are not included. Optional NVML use relies on the user's installed driver and its applicable license; the [NVML linking permission](NVML-EXCEPTION.txt) applies to DoWATT and does not grant rights over NVIDIA software.

This notice does not waive third-party intellectual property rights, guarantee non-infringement, replace applicable licenses, or impose additional restrictions on rights granted by the GNU GPL v3. If you believe this repository includes material used without permission, please contact the maintainer through the [repository](https://github.com/dogukansahil/DoWATT) and identify the material and the basis of your claim.
