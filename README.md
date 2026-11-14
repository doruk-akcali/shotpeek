# ShotPeek

macOS-style screenshot preview for Windows 11. Take a screenshot and a thumbnail slides into the bottom-right corner, where you can drag it straight into a chat, an email or a folder — like on a Mac.

- **F6** freezes the screen instantly — ShotPeek is already running, so unlike Snipping Tool there is no app to wake up. Drag a box (its size in pixels shows below it), release to capture; **Esc** or right-click cancels. The shot is saved as `Screenshot YYYY-MM-DD HHMMSS.png` in your Screenshots folder and copied to the clipboard
- Every new image in your **Screenshots** folder (ShotPeek's own captures, and Snipping Tool's: Win+Shift+S and Print Screen) slides in as a thumbnail, rests for 6 seconds, then slides away to the right; it stays while the mouse is on it
- **Drag** the thumbnail into any app to drop the image file · **click** to open it · **swipe right** to dismiss · **right-click** for Open / Show in folder / Copy / Close / Quit

One small C file, no dependencies beyond Windows itself (GDI+, OLE drag-and-drop). The animation is paced by `DwmFlush`, so it runs at the display's refresh rate.

F6 is taken system-wide while ShotPeek runs, so apps no longer receive it.

## Build

With [w64devkit](https://github.com/skeeto/w64devkit):

```
gcc -std=c99 -Os -municode -mwindows -s -o shotpeek.exe src/shotpeek.c -lgdiplus -lole32 -luuid -lshell32 -lshcore -ldwmapi -lwinmm -lgdi32
```

## Start at login

```powershell
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name ShotPeek -Value '"C:\path\to\shotpeek.exe" --autorun'
```
