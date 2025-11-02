{
    // Small helper for neat multi-line messages in console
    // eslint-disable-next-line no-extend-native
    // @ts-ignore
    String.prototype.dedent = function () {
        return this.split('\n')
            .map((l: string) => l.trim())
            .join('\n');
    };

    // Unregister previous instance if script is reloaded
    // @ts-ignore
    if (mirv._mirv_script_lock !== undefined) mirv._mirv_script_lock.unregister();

    let active = false;
    let halftimeSec = 0.10; // lock tracking half-time fed into MirvInput
    let targetPawnIndex = -1;
    let pendingAcquire = false; // acquire target on next frame
    let lastOutPitch = 0.0;
    let lastOutYaw = 0.0;
    let hasLastOut = false;
    let restoreHalfTimeAng: number | null = null; // value to restore when disabling
    let restoreSmoothEnabled: boolean | null = null; // smooth enabled restore
    let restoreMouseYawSpeed: number | null = null;
    let restoreMousePitchSpeed: number | null = null;
    let blendInSec = 0.10; // time to blend current halfTimeAng -> lock halfTimeAng
    let blendActive = false;
    let blendStartTime: number | null = null;
    let blendStartHalfTime = 0.0;
    let disableOnDead = false; // if true, disable lock when target health is 0

    const disableLock = () => {
        if (!active) {
            //mirv.message('[mirv_lock] Already disabled.\n');
            return;
        }
        if (hasLastOut) {
            // setMirvInputAngles expects (pitch, yaw, roll)
            mirv.setMirvInputAngles(lastOutPitch, lastOutYaw, 0);
        }
        if (restoreHalfTimeAng !== null) {
            mirv.setMirvInputHalfTimeAng(restoreHalfTimeAng);
        } else {
            mirv.warning('[mirv_lock] Note: No restore value for mirv_input cfg smooth halfTimeAng set. Use "mirv_lock halftimeang <seconds>" to set it.\n');
        }
        if (restoreSmoothEnabled !== null) {
            mirv.setMirvInputSmoothEnabled(restoreSmoothEnabled);
        }
        active = false;
        targetPawnIndex = -1;
        pendingAcquire = false;
        phase = 0;
        mirv.message('[mirv_lock] Disabled.\n');
        // Restore mouse turn speeds
        if (restoreMouseYawSpeed !== null) mirv.setMirvInputMouseYawSpeed(restoreMouseYawSpeed);
        if (restoreMousePitchSpeed !== null) mirv.setMirvInputMousePitchSpeed(restoreMousePitchSpeed);
    };

    const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));
    const deg2rad = (d: number) => (d * Math.PI) / 180.0;

    const angleNormalize180 = (a: number): number => {
        while (a > 180.0) a -= 360.0;
        while (a < -180.0) a += 360.0;
        return a;
    };

    const angleDelta = (target: number, current: number): number => angleNormalize180(target - current);

    const halfExp = (halftime: number, dt: number): number => {
        halftime = halftime <= 0.0001 ? 0.0001 : halftime;
        return 1.0 - Math.pow(0.5, dt / halftime);
    };

    const forwardFromAngles = (pitchDeg: number, yawDeg: number): [number, number, number] => {
        const p = deg2rad(pitchDeg);
        const y = deg2rad(yawDeg);
        const cp = Math.cos(p), sp = Math.sin(p);
        const cy = Math.cos(y), sy = Math.sin(y);
        // Source-style (X fwd, Y right, Z up), pitch positive looks down -> forward.z = -sin(pitch)
        return [cp * cy, cp * sy, -sp];
    };

    const normalize3 = (v: [number, number, number]): null | [number, number, number] => {
        const len = Math.hypot(v[0], v[1], v[2]);
        if (!(len > 1e-6)) return null;
        return [v[0] / len, v[1] / len, v[2] / len];
    };

    // Find closest pawn to provided view angles (camera center), in front of camera
    const findClosestPawnToCenter = (e: mirv.OnCViewRenderSetupViewArgs, viewPitch: number, viewYaw: number): number => {
        const highest = mirv.getHighestEntityIndex();
        if (highest < 0) return -1;

        const fwd = forwardFromAngles(viewPitch, viewYaw);
        let bestIdx = -1;
        let bestAngle = Number.POSITIVE_INFINITY;

        for (let idx = 0; idx <= highest; ++idx) {
            const ctrl = mirv.getEntityFromIndex(idx);
            if (!ctrl || !ctrl.isPlayerController()) continue;

            // Skip spectators / non-active controllers if available
            const observerMode = ctrl.getObserverMode?.();
            if (typeof observerMode === 'number' && observerMode !== 0) continue;

            const team = ctrl.getTeam?.();
            if (typeof team === 'number' && !(team === 2 || team === 3)) continue;

            const pawnHandle = ctrl.getPlayerPawnHandle();
            if (!mirv.isHandleValid(pawnHandle)) continue;

            const pawnIdx = mirv.getHandleEntryIndex(pawnHandle);
            const pawn = mirv.getEntityFromIndex(pawnIdx);
            if (!pawn || !pawn.isPlayerPawn()) continue;

            // Skip dead players
            if (pawn.getHealth() === 0) continue;

            const eye = pawn.getRenderEyeOrigin();
            const to: [number, number, number] = [
                eye[0] - e.currentView.x,
                eye[1] - e.currentView.y,
                eye[2] - e.currentView.z
            ];
            const toN = normalize3(to);
            if (!toN) continue;

            const dot = fwd[0] * toN[0] + fwd[1] * toN[1] + fwd[2] * toN[2];
            if (dot <= 0) continue; // behind camera

            const ang = Math.acos(clamp(dot, -1.0, 1.0)); // radians
            if (ang < bestAngle) {
                bestAngle = ang;
                bestIdx = pawnIdx;
            }
        }

        return bestIdx;
    };

    // Simple phase for enabling state
    let phase: 0 | 1 | 2 = 0; // 0=idle,1=engaging,2=locked

    // Per-frame updater using MirvInput directly, no view override
    let lastUpdateTime: number | null = null;
    const onFrame: mirv.OnClientFrameStageNotify = (e) => {
        if (!e.isBefore) return; // run before stage to affect this frame
        const camCtrlEnabled = mirv.getMirvInputCameraControlMode();
        // If user disabled mirv_input while lock is active, automatically stop the lock
        if (!camCtrlEnabled) {
            if (active) disableLock();
            return;
        }
        if (!active && phase === 0) { lastUpdateTime = mirv.getCurTime(); return; }

        const cur = mirv.getCurTime();
        const dt = lastUpdateTime !== null ? Math.max(0, cur - lastUpdateTime) : 0;
        lastUpdateTime = cur;

        if (phase === 1) phase = 2; // one-frame engage; MirvInput will handle smoothing

        const cam = mirv.getLastCameraData();

        // Blend MirvInput halfTimeAng from original to lock value if requested
        if (blendActive && blendStartTime !== null) {
            const curTime = mirv.getCurTime();
            const t = Math.max(0, Math.min(1, (curTime - blendStartTime) / Math.max(0.0001, blendInSec)));
            const newHalf = blendStartHalfTime + (halftimeSec - blendStartHalfTime) * t;
            mirv.setMirvInputHalfTimeAng(newHalf);
            if (t >= 1) {
                blendActive = false;
                blendStartTime = null;
            }
        }

        if (pendingAcquire || targetPawnIndex < 0) {
            targetPawnIndex = findClosestPawnToCenter({ currentView: cam, lastView: cam } as any, cam.rX, cam.rY);
            pendingAcquire = false;
            if (targetPawnIndex < 0) {
                mirv.warning('[mirv_lock] No target found in view.\n');
                active = false; phase = 0;
                return;
            }
            const ctrl = mirv.getEntityFromIndex(targetPawnIndex);
            if (ctrl) {
                const name = ctrl.getDebugName() || ctrl.getClassName();
                mirv.message(`[mirv_lock] Target acquired: #${targetPawnIndex} ${name}\n`);
            }
        }

        const pawn = targetPawnIndex >= 0 ? mirv.getEntityFromIndex(targetPawnIndex) : null;
        if (!pawn || !pawn.isValid() || !pawn.isPlayerPawn() || (disableOnDead && pawn.getHealth() === 0)) {
            // Hard disable (no blend-out)
            disableLock();
            return;
        }

        // Desired look direction from current camera origin to pawn eye
        let desiredYaw = cam.rY;
        let desiredPitch = cam.rX;
        if (phase !== 0) {
            const eye = pawn.getRenderEyeOrigin();
            const dx = eye[0] - cam.x;
            const dy = eye[1] - cam.y;
            const dz = eye[2] - cam.z;
            const hdist = Math.sqrt(dx * dx + dy * dy);
            desiredYaw = (Math.atan2(dy, dx) * 180.0) / Math.PI;
            desiredPitch = (Math.atan2(-dz, hdist) * 180.0) / Math.PI;
        }

        // Drive MirvInput target angles and let MirvInput smoothing seek
        const yawDelta = Math.abs(angleDelta(desiredYaw, cam.rY));
        const pitchDelta = Math.abs(angleDelta(desiredPitch, cam.rX));
        if (yawDelta > 0.001 || pitchDelta > 0.001) {
            // setMirvInputAngles expects (pitch, yaw, roll)
            mirv.setMirvInputAngles(desiredPitch, desiredYaw, 0);
            lastOutPitch = desiredPitch;
            lastOutYaw = desiredYaw;
            hasLastOut = true;
        }
    };

    // Install frame hook
    mirv.onClientFrameStageNotify = onFrame;

    // Create command
    // @ts-ignore
    mirv._mirv_script_lock = new AdvancedfxConCommand((args: AdvancedfxConCommandArgs) => {
        const argc = args.argC();
        const cmd = args.argV(0);
        if (argc >= 2) {
            const sub = args.argV(1).toLowerCase();
            if (sub === 'toggle') {
                if (!active) {
                    active = true;
                    pendingAcquire = true;
                    phase = 1; // engaging
                    // Ensure MirvInput camera is active and smoothing configured
                    mirv.setMirvInputCameraControlMode(true);
                    // Enable smoothing and set its half-time for angles
                    const curHalfTimeAng = mirv.getMirvInputHalfTimeAng();
                    const curSmoothEnabled = mirv.getMirvInputSmoothEnabled();
                    if (restoreHalfTimeAng === null) restoreHalfTimeAng = curHalfTimeAng;
                    if (restoreSmoothEnabled === null) restoreSmoothEnabled = curSmoothEnabled;
                    if (!curSmoothEnabled) mirv.setMirvInputSmoothEnabled(true);
                    // Start blending halfTimeAng if requested
                    if (blendInSec > 0) {
                        blendActive = true;
                        blendStartTime = mirv.getCurTime();
                        blendStartHalfTime = curHalfTimeAng;
                        // immediately apply start value to avoid a jump
                        mirv.setMirvInputHalfTimeAng(blendStartHalfTime);
                    } else {
                        mirv.setMirvInputHalfTimeAng(halftimeSec);
                    }
                    // Freeze user yaw/pitch input while locked to avoid fighting
                    if (restoreMouseYawSpeed === null) restoreMouseYawSpeed = mirv.getMirvInputMouseYawSpeed();
                    if (restoreMousePitchSpeed === null) restoreMousePitchSpeed = mirv.getMirvInputMousePitchSpeed();
                    mirv.setMirvInputMouseYawSpeed(0);
                    mirv.setMirvInputMousePitchSpeed(0);
                    mirv.message(`[mirv_lock] Enabled (halftime ${halftimeSec.toFixed(3)}s).\n`);
                } else {
                    disableLock();
                }
                return;
            } else if (sub === 'stop') {
                disableLock();
                return;
            } else if (sub === 'halftime') {
                if (argc >= 3) {
                    const v = parseFloat(args.argV(2));
                    if (!isNaN(v) && v >= 0) {
                        halftimeSec = v;
                        // If currently active, update MirvInput immediately
                        if (active) mirv.setMirvInputHalfTimeAng(halftimeSec);
                        mirv.message(`[mirv_lock] halftime = ${halftimeSec.toFixed(3)}s\n`);
                        return;
                    }
                }
                mirv.message(
                    `${cmd} halftime <seconds> - Set angle smoothing half-time.\n` +
                    `Current value: ${halftimeSec}\n`
                );
                return;
            } else if (sub === 'blend') {
                if (argc >= 3) {
                    const v = parseFloat(args.argV(2));
                    if (!isNaN(v) && v >= 0) {
                        blendInSec = v;
                        mirv.message(`[mirv_lock] blend = ${blendInSec.toFixed(3)}s\n`);
                        return;
                    }
                }
                mirv.message(
                    `${cmd} blend <seconds> - Blend from current mirv_input halfTimeAng to lock halfTimeAng over given time.\n` +
                    `Current value: ${blendInSec}\n`
                );
                return;
            } else if (sub === 'nodead') {
                // Enable/disable auto-disabling lock when target dies (health==0)
                if (argc >= 3) {
                    const v = parseInt(args.argV(2));
                    if (!isNaN(v) && (v === 0 || v === 1)) {
                        disableOnDead = v === 1;
                        mirv.message(`[mirv_lock] nodead = ${disableOnDead ? 1 : 0}\n`);
                        return;
                    }
                }
                mirv.message(
                    `${cmd} nodead 1|0 - Disable lock when target health is 0 (default 0).\n` +
                    `Current value: ${disableOnDead ? 1 : 0}\n`
                );
                return;
            }
        }

        mirv.message(
            `Usage:\n` +
            `${cmd} toggle - Toggle camera lock on the player at crosshair.\n` +
            `${cmd} stop - Disable camera lock (restore smoothing).\n` +
            `${cmd} halftime <seconds> - Set smoothing half-time for lock angles.\n` +
            `${cmd} blend <seconds> - Blend current halfTimeAng to lock halfTimeAng over time.\n` +
            `${cmd} nodead 1|0 - Disable lock when target health is 0 (default 0).\n`
        );
    });

    // Register command
    // @ts-ignore
    mirv._mirv_script_lock.register('mirv_lock', 'Lock camera on aimed player (angles), with halftime smoothing.');
}
