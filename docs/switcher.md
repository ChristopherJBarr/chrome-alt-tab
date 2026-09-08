# Acceptance checks

Use two Chrome profiles with ordinary web pages and an unrelated app such as File Explorer. Allow previews if testing them. Start with a helper restart while Chrome is in the background to exercise recovery without a previously learned window mapping. Also test after visiting a tab in each window.

1. Hold Alt and tap Tab. Check the rounded panel, title/icon headers and selection outline at 100%, 150% and 200% display scaling, including different monitors and light/dark Windows themes.
2. With Chrome connected, use Ctrl+Alt+F8 to hold the live switcher open. Place a window with contrasting light/dark or coloured areas behind the panel. Confirm the gaps between cards show softened colour transitions, not a uniform grey fill. Verify the panel has no title bar or resize border. Repeat after dismissal; also verify normal held Alt+Tab. With Battery Saver on or transparency effects disabled, expect an opaque themed panel rather than blur or a black fallback. Reopen after the system permits effects again to verify blur resumes.
3. Visit tabs A, B and C, then start Alt+Tab. The current tab should be first and B selected next. Image refreshes must not change their order. Reverse with Shift+Tab and cycle across page boundaries. Check arrow keys and Home/End, including up/down alignment across cards of different widths.
4. Select a tab in the other profile. Confirm the exact profile, window and tab activate. Repeat with the same page title in both profiles and record any ambiguity.
5. Switch from Chrome to Explorer and back. Check that activation succeeds without taskbar flashing or an extra helper entry. Repeat immediately after restarting the helper, before manually revisiting Chrome; use distinct titles first, then duplicate titles to check that recovery does not guess the wrong profile.
6. Release Alt to choose. Press Esc to cancel. Click a card to choose. On a disposable tab, hover and click its close button; confirm only that tab closes. Test a disposable ordinary window too. Confirm cancellation keeps the previous application and leaves no popup.
7. Visit a page after enabling previews. Confirm its card contains the new image. Revoke preview permission and confirm cached images clear. Incognito and internal browser pages must not be captured.
8. Close/move/rename tabs, open a second Chrome window and restart one profile. Check stale cards disappear and the other profile continues working.
9. Suspend with Ctrl+Alt+F10; native Alt+Tab must work. Resume with Ctrl+Alt+F9. Ctrl+Alt+Tab must always reach the native sticky switcher.
10. Disable both extensions. Confirm the tray reports Waiting for Chrome and native switching works. Choose Quit and confirm all helper processes exit. Re-enable and verify recovery.

Before declaring production stability, also cover sleep/resume, remote desktop, high contrast and assistive technology, virtual desktops, elevated applications, games, large tab counts, rapid switching and mixed-DPI monitor transitions. The current renderer does not supply a full UI Automation card tree or native snap-group behaviour.

Automated model, extension and IPC tests cover important data and lifecycle rules, but they do not substitute for these desktop checks.

Installer acceptance: install the release ZIP, check the HKCU Run command, start it with Chrome closed, pause/resume and quit through the tray, restart using the same command as sign-in, then uninstall through Installed apps. Verify local files, native-host registration, shortcut and startup entry are removed. Repeat installation and upgrade. A simulated sign-in command is not a substitute for a real reboot test; record which was performed.
