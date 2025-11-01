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
    let halftimeSec = 0.10; // lock tracking half-time (angles)
    // Additional engage blending for natural feel
    let blendInHalftimeSec = 0.10;
    let targetPawnIndex = -1;
    let pendingAcquire = false; // acquire target on next view callback
    let lastOutPitch = 0.0;
    let lastOutYaw = 0.0;
    let hasLastOut = false;
    let restoreHalfTimeAng: number | null = null; // value to restore when disabling
    let disableOnDead = false; // if true, disable lock when target health is 0

    const disableLock = () => {
        if (!active) {
            //mirv.message('[mirv_lock] Already disabled.\n');
            return;
        }
        if (hasLastOut) {
            mirv.exec(`mirv_input angles ${lastOutPitch} ${lastOutYaw} 0`);
        }
        if (restoreHalfTimeAng !== null) {
            mirv.exec(`mirv_input cfg smooth halfTimeAng ${restoreHalfTimeAng}`);
        } else {
            mirv.warning('[mirv_lock] Note: No restore value for mirv_input cfg smooth halfTimeAng set. Use "mirv_lock halftimeang <seconds>" to set it.\n');
        }
        active = false;
        targetPawnIndex = -1;
        pendingAcquire = false;
        phase = 0;
        blend = 0.0;
        mirv.message('[mirv_lock] Disabled.\n');
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

    // Blend factor and phase for smooth enable
    let blend = 0.0; // 0..1
    let phase: 0 | 1 | 2 = 0; // 0=idle,1=engaging,2=locked

    const onView: mirv.OnCViewRenderSetupView = (e) => {
        if (!active && phase === 0) return undefined;

        const dt = Math.max(0, e.absTime ?? 0);
        // Advance blend based on phase
        if (phase === 1) {
            const sfB = halfExp(blendInHalftimeSec, dt);
            blend = blend + (1.0 - blend) * sfB;
            if (blend > 0.999) { blend = 1.0; phase = 2; }
        }

        if (pendingAcquire || targetPawnIndex < 0) {
            // Use lastView angles for stable selection corresponding to what was on screen
            targetPawnIndex = findClosestPawnToCenter(e, e.lastView.rX, e.lastView.rY);
            pendingAcquire = false;
            if (targetPawnIndex < 0) {
                mirv.warning('[mirv_lock] No target found in view.\n');
                active = false; phase = 0; blend = 0.0;
                return undefined;
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
            return undefined;
        }

        // Compute desired look angles only while engaging/locked and when pawn is valid.
        // Fallback to lastView angles otherwise (disengaging path will cross-fade).
        let desiredYaw = e.lastView.rY;
        let desiredPitch = e.lastView.rX;
        if (pawn && (phase === 1 || phase === 2)) {
            const eye = pawn.getRenderEyeOrigin();
            const dx = eye[0] - e.currentView.x;
            const dy = eye[1] - e.currentView.y;
            const dz = eye[2] - e.currentView.z;
            const hdist = Math.sqrt(dx * dx + dy * dy);
            desiredYaw = (Math.atan2(dy, dx) * 180.0) / Math.PI;
            desiredPitch = (Math.atan2(-dz, hdist) * 180.0) / Math.PI;
        }

        // absTime is the frame delta time (see AfxHookSource2/main.cpp)
        const sfTrack = halfExp(halftimeSec, dt);
        const sf = sfTrack * Math.max(0.0, Math.min(1.0, blend));
        const outYaw = e.lastView.rY + angleDelta(desiredYaw, e.lastView.rY) * sf;
        const outPitch = e.lastView.rX + angleDelta(desiredPitch, e.lastView.rX) * sf;

        if (phase === 1 || phase === 2) {
            lastOutPitch = outPitch;
            lastOutYaw = outYaw;
            hasLastOut = true;
        }

        // Keep MirvInput's internal angle state in sync so movement (WASD)
        // happens in the locked direction and disabling lock does not snap back.
        // Update only when there is a noticeable change to reduce spam.
        if (active) {
            const yawDelta = Math.abs(angleDelta(outYaw, e.currentView.rY));
            const pitchDelta = Math.abs(angleDelta(outPitch, e.currentView.rX));
            if (yawDelta > 0.001 || pitchDelta > 0.001) {
                mirv.exec(`mirv_input angles ${outPitch} ${outYaw} 0`);
            }
        }

        return { rX: outPitch, rY: outYaw, rZ: 0 };
    };

    // Install hook immediately; it only acts when active=true
    mirv.onCViewRenderSetupView = onView;

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
                    blend = 0.0;
                    // Disable MirvInput angle smoothing while locked to avoid lag/snap on release
                    mirv.exec('mirv_input cfg smooth halfTimeAng 0');
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
                        mirv.message(`[mirv_lock] halftime = ${halftimeSec.toFixed(3)}s\n`);
                        return;
                    }
                }
                mirv.message(
                    `${cmd} halftime <seconds> - Set angle smoothing half-time.\n` +
                    `Current value: ${halftimeSec}\n`
                );
                return;
            } else if (sub === 'halftimeang') {
                if (argc >= 3) {
                    const v = parseFloat(args.argV(2));
                    if (!isNaN(v) && v >= 0) {
                        restoreHalfTimeAng = v;
                        mirv.message(`[mirv_lock] restore halfTimeAng = ${restoreHalfTimeAng.toFixed(3)}s\n`);
                        return;
                    }
                }
                mirv.message(
                    `${cmd} halftimeang <seconds> - Set mirv_input cfg smooth halfTimeAng to restore on disable.\n` +
                    `Current restore value: ${restoreHalfTimeAng === null ? 'unset' : restoreHalfTimeAng}\n`
                );
                return;
            } else if (sub === 'blend') {
                // Set engage blend half-time only: mirv_lock blend <inSeconds>
                if (argc >= 3) {
                    const inV = parseFloat(args.argV(2));
                    if (!isNaN(inV) && inV >= 0) {
                        blendInHalftimeSec = inV;
                        mirv.message(`[mirv_lock] blend-in half-time = ${blendInHalftimeSec.toFixed(3)}s\n`);
                        return;
                    }
                }
                mirv.message(
                    `${cmd} blend <inSeconds> - Set engage (blend-in) half-time.\n` +
                    `Current: in=${blendInHalftimeSec}\n`
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
            `${cmd} halftimeang <seconds> - Set mirv_input cfg smooth halfTimeAng to restore on disable.\n` +
            `${cmd} blend <inSeconds> - Set engage (blend-in) half-time.\n` +
            `${cmd} nodead 1|0 - Disable lock when target health is 0 (default 0).\n`
        );
    });

    // Register command
    // @ts-ignore
    mirv._mirv_script_lock.register('mirv_lock', 'Lock camera on aimed player (angles), with halftime smoothing.');
}
