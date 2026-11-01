# Third-party notices

See [COPYRIGHT.md](COPYRIGHT.md) and [the provenance review](docs/PROVENANCE.md) for scope. This is a modified, independent project, not an upstream release. Upstream code and notices are not claimed as our original work. The CareyScott license caveat below is that upstream project's own statement; its MIT grant does not license its unlicensed ancestors.

WinStadia source reviewed at 2778707f873ca7111586579b2de8bf418b6fe829 is credited to aisk and its contributors under GPLv3. The complete GPL text is supplied in LICENSE. Switch2Native changes the device model, transport ownership, reports and runtime behavior.

This project is distributed under GPL-3.0. The Xbox-compatible HID descriptor and UMDF dispatch design are adapted from [aisk/WinStadia](https://github.com/aisk/WinStadia), GPL-3.0. Our driver uses a root virtual device and public Bluetooth APIs; it does not use WinStadia's private GATT interfaces.

Nintendo protocol facts and adapted initialization, button layout, calibration, and rumble encoding come from the sources below. Their original licenses follow. No upstream executables or installers are bundled.

## CareyScott/switch2controllerpc
https://github.com/CareyScott/switch2controllerpc

MIT License

Copyright (c) 2026 Scott Carey

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

This project is a derivative work of:
  - https://github.com/TommyWabg/switch2-controllers-windows10-gyro (Windows 10/11 gyro fork)
  - https://github.com/Nadeflore/switch2-controllers (original switch2-controllers project)

Both upstream projects ship without an explicit license file at the time of
forking. This MIT license applies only to the modifications and additions made
in this repository. Upstream code is incorporated in good faith with
attribution; if either upstream author objects, please open an issue.

## Altureus/Windows-Switch2-Pro-Controller
https://github.com/Altureus/Windows-Switch2-Pro-Controller

MIT License

Copyright (c) 2026 amccomb

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## SDL
https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_switch2.c

Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

SDL's Switch 2 source also credits code and logic contributed by Valve Corporation under the SDL zlib license. Switch2Native's motion/rumble adaptations are altered versions, including clock measurement and a reduced rumble amplitude ceiling.

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
## Installer runtime

The optional offline installer is built using unmodified Inno Setup 6.7.4 by Jordan Russell and Martijn Laan. Its license is preserved in [installer/INNO-LICENSE.txt](installer/INNO-LICENSE.txt) and in installed notices. Inno Setup is separately licensed; the project does not claim authorship of its runtime. The compiler is downloaded only during maintainer/CI builds from a hash-pinned, publisher-verified official release; it is not required on end-user PCs. Upstream: <https://jrsoftware.org/> and <https://github.com/jrsoftware/issrc/tree/is-6_7_4>.
