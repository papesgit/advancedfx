{
    // Small helper to format usage text like other examples
    // eslint-disable-next-line no-extend-native
    // @ts-ignore
    String.prototype.dedent = function () {
        return this.split('\n').map((l: string) => l.trim()).join('\n');
    };

    // Unregister previous command if reloaded
    // @ts-ignore
    if ((mirv as any)._mirv_bird !== undefined) (mirv as any)._mirv_bird.unregister();

    type Vec3 = [number, number, number];

    const v = {
        add: (a: Vec3, b: Vec3): Vec3 => [a[0] + b[0], a[1] + b[1], a[2] + b[2]],
        sub: (a: Vec3, b: Vec3): Vec3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]],
        scale: (a: Vec3, s: number): Vec3 => [a[0] * s, a[1] * s, a[2] * s],
        len: (a: Vec3): number => Math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]),
        norm: (a: Vec3): Vec3 => {
            const l = Math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            if (l <= 1e-8) return [0, 0, 0];
            return [a[0] / l, a[1] / l, a[2] / l];
        },
        dot: (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2],
    };

    function angleNormalize(a: number): number { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
    function angleDelta(a: number, b: number): number { return angleNormalize(a - b); }

    // Critically damped smoothing, mirrors C++-style SmoothDamp
    function smoothDamp1D(current: number, target: number, vel: number, smoothTime: number, maxSpeed: number, dt: number): { value: number; vel: number } {
        smoothTime = smoothTime < 1e-4 ? 1e-4 : smoothTime;
        const omega = 2.0 / smoothTime;
        const x = omega * dt;
        const exp = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
        let change = current - target;
        const originalTo = target;
        const maxChange = maxSpeed * smoothTime;
        if (maxChange > 0.0) {
            if (change > maxChange) change = maxChange; else if (change < -maxChange) change = -maxChange;
        }
        target = current - change;
        const temp = (vel + omega * change) * dt;
        const newVel = (vel - omega * temp) * exp;
        let output = target + (change + temp) * exp;
        if (((originalTo - current) > 0.0) === (output > originalTo)) {
            output = originalTo;
            const outVel = (output - originalTo) / dt;
            return { value: output, vel: outVel };
        }
        return { value: output, vel: newVel };
    }

    function smoothDampAngle1D(current: number, target: number, vel: number, smoothTime: number, maxSpeed: number, dt: number): { value: number; vel: number } {
        const delta = angleDelta(target, current);
        const r = smoothDamp1D(current, current + delta, vel, smoothTime, maxSpeed, dt);
        return { value: angleNormalize(r.value), vel: r.vel };
    }

    function dirToAngles(dir: Vec3): { pitch: number; yaw: number; roll: number } {
        const n = v.norm(dir);
        const yaw = Math.atan2(n[1], n[0]) * 180 / Math.PI;
        const pitch = -Math.asin(n[2]) * 180 / Math.PI; // forward.z = -sin(pitch)
        return { pitch, yaw, roll: 0 };
    }

    function snapYawToCardinal(yaw: number): number {
        // Normalize, snap to nearest 90°, then re-normalize to [-180,180]
        const n = angleNormalize(yaw);
        const snapped = Math.round(n / 90) * 90;
        return angleNormalize(snapped);
    }

    // Helpers to get entities
    function getPawnFromControllerIndex(idx: number): mirv.Entity | null {
        const c = mirv.getEntityFromIndex(idx);
        if (!c || !c.isPlayerController()) return null;
        try {
            const h = c.getPlayerPawnHandle();
            if (h && mirv.isHandleValid(h)) {
                const pawnIdx = mirv.getHandleEntryIndex(h);
                const p = mirv.getEntityFromIndex(pawnIdx);
                if (p && p.isPlayerPawn()) return p;
            }
        } catch { /* ignore */ }
        return null;
    }

    function getObservedControllerIndex(): number | null {
        try {
            const localCtrl = mirv.getEntityFromSplitScreenPlayer(0);
            if (!localCtrl) return null;
            const pawnIdx = mirv.getHandleEntryIndex(localCtrl.getPlayerPawnHandle());
            const localPawn = mirv.getEntityFromIndex(pawnIdx);
            if (!localPawn) return null;
            const tgtPawnIdx = mirv.getHandleEntryIndex(localPawn.getObserverTargetHandle());
            const tgtPawn = mirv.getEntityFromIndex(tgtPawnIdx);
            if (!tgtPawn) return null;
            const ctrlIdx = mirv.getHandleEntryIndex(tgtPawn.getPlayerControllerHandle());
            return Number.isFinite(ctrlIdx) ? ctrlIdx : null;
        } catch { return null; }
    }

    function getNameForControllerIndex(idx: number): string {
        try {
            const c = mirv.getEntityFromIndex(idx);
            const s = c?.getSanitizedPlayerName();
            if (s && s.length) return s;
        } catch { /* ignore */ }
        return `#${idx}`;
    }

    enum Phase { Idle = 0, AscendA = 1, HoldA = 2, ToB = 3, HoldB = 4, DescendB = 5 }

    const state = {
        active: false,
        phase: Phase.Idle,
        fromIdx: -1,
        toIdx: -1,
        height: 500.0,
        holdTime: 1.0,
        phaseStart: 0.0,
        // Kinematics
        pos: [0, 0, 0] as Vec3,
        vel: [0, 0, 0] as Vec3,
        ang: [0, 0, 0] as Vec3,
        angVel: [0, 0, 0] as Vec3,
        initialized: false,
        downYaw: 0.0,
        downYawInit: false,
        // Smoothing params
        linSpeed: 500.0, // units/s nominal
        minSpeed: 300.0, // units/s minimum while far from target
        velSmoothTime: 0.5, // seconds
        angSmoothTime: 0.25, // seconds
        // decel and angle blending radii are derived from height each frame
        // Single arrival margin used for horizontal and vertical checks
        margin: 5.0,
    };

    function getBirdTargetForController(idx: number): { pos: Vec3; lookAt: Vec3 } | null {
        const pawn = getPawnFromControllerIndex(idx);
        if (!pawn) return null;
        const eye = pawn.getRenderEyeOrigin();
        // birds-eye above eyes
        const pos: Vec3 = [eye[0], eye[1], eye[2] + state.height];
        // look at player center (use origin for better centering)
        const org = pawn.getOrigin();
        const lookAt: Vec3 = [org[0], org[1], org[2]];
        return { pos, lookAt };
    }

    function getEyesForController(idx: number): { pos: Vec3; ang: Vec3 } | null {
        const c = mirv.getEntityFromIndex(idx);
        if (!c || !c.isPlayerController()) return null;
        try {
            const h = c.getPlayerPawnHandle();
            if (h && mirv.isHandleValid(h)) {
                const pawnIdx = mirv.getHandleEntryIndex(h);
                const pawn = mirv.getEntityFromIndex(pawnIdx);
                if (pawn && pawn.isPlayerPawn()) {
                    const eo = pawn.getRenderEyeOrigin();
                    const ea = pawn.getRenderEyeAngles();
                    return { pos: [eo[0], eo[1], eo[2]], ang: [ea[0], ea[1], ea[2]] };
                }
            }
        } catch { /* ignore */ }
        return null;
    }

    function beginPhase(p: Phase) {
        state.phase = p;
        state.phaseStart = mirv.getCurTime();
    }

    function onView(e: mirv.OnCViewRenderSetupViewArgs): mirv.OnCViewRenderSetupViewSet | undefined {
        if (!state.active) return;

        // Initialize camera state on first frame
        if (!state.initialized) {
            state.pos = [e.currentView.x, e.currentView.y, e.currentView.z];
            state.ang = [e.currentView.rX, e.currentView.rY, e.currentView.rZ];
            const dt0 = e.absTime > 1e-6 ? e.absTime : 1 / 64.0;
            const dv: Vec3 = [e.currentView.x - e.lastView.x, e.currentView.y - e.lastView.y, e.currentView.z - e.lastView.z];
            state.vel = v.scale(dv, 1 / dt0);
            state.angVel = [
                angleDelta(e.currentView.rX, e.lastView.rX) / dt0,
                angleDelta(e.currentView.rY, e.lastView.rY) / dt0,
                angleDelta(e.currentView.rZ, e.lastView.rZ) / dt0,
            ];
            state.initialized = true;
            if (!state.downYawInit) {
                state.downYaw = snapYawToCardinal(e.currentView.rY);
                state.downYawInit = true;
            }
        }

        const dt = Math.max(1 / 128.0, Math.min(e.absTime || 0.0, 0.25));
        let targetPos: Vec3 | null = null;
        let targetAngles: Vec3 | null = null;

        // Compute targets based on phase
        if (state.phase === Phase.AscendA || state.phase === Phase.HoldA) {
            const tgt = getBirdTargetForController(state.fromIdx);
            if (!tgt) {
                // Abort if target lost for some time
                if ((mirv.getCurTime() - state.phaseStart) > 3.0) {
                    mirv.warning('mirv_bird: source player not available, aborting.\n');
                    state.active = false; state.initialized = false; mirv.onCViewRenderSetupView = undefined; return;
                }
                return;
            }
            // Hard lock horizontally to player; top-down yaw fixed to world axes
            targetPos = [tgt.pos[0], tgt.pos[1], tgt.pos[2]];
            const pitchDown = 89.9;
            targetAngles = [pitchDown, state.downYaw, 0];
        } else if (state.phase === Phase.ToB || state.phase === Phase.HoldB) {
            // For ToB and HoldB, always recompute destination bird position (live lock)
            const tgt = getBirdTargetForController(state.toIdx);
            if (!tgt) {
                if ((mirv.getCurTime() - state.phaseStart) > 3.0) {
                    mirv.warning('mirv_bird: destination player not available, aborting.\n');
                    state.active = false; state.initialized = false; mirv.onCViewRenderSetupView = undefined; return;
                }
                return;
            }
            targetPos = [tgt.pos[0], tgt.pos[1], tgt.pos[2]];
            const pitchDown = 89.9; // strict top-down between bird points
            targetAngles = [pitchDown, state.downYaw, 0];
        } else if (state.phase === Phase.DescendB) {
            const tgt = getEyesForController(state.toIdx);
            if (!tgt) {
                if ((mirv.getCurTime() - state.phaseStart) > 3.0) {
                    mirv.warning('mirv_bird: destination POV not available, aborting.\n');
                    state.active = false; state.initialized = false; mirv.onCViewRenderSetupView = undefined; return;
                }
                return;
            }
            targetPos = tgt.pos;
            // Keep top-down until close, then blend to player POV angles
            const topDown: Vec3 = [89.9, state.downYaw, 0];
            const toTarget = v.sub(targetPos, state.pos);
            const distD = v.len(toTarget);
            const blendRadius = Math.max(1.0, state.height * 0.5);
            const tBlend = Math.max(0, Math.min(1, 1.0 - (distD / blendRadius)));
            const lerpAngle = (a: number, b: number, t: number) => angleNormalize(a + angleDelta(b, a) * t);
            targetAngles = [
                lerpAngle(topDown[0], tgt.ang[0], tBlend),
                lerpAngle(topDown[1], tgt.ang[1], tBlend),
                lerpAngle(topDown[2], tgt.ang[2], tBlend),
            ];
        }

        if (!targetPos || !targetAngles) return;

        // Update position: hard lock during holds, lock XY during ascend, vector-smooth with min-speed floor on path phases
        if (state.phase === Phase.HoldA || state.phase === Phase.HoldB) {
            state.pos = [targetPos[0], targetPos[1], targetPos[2]];
            state.vel = [0, 0, 0];
        } else if (state.phase === Phase.AscendA) {
            const sz = smoothDamp1D(state.pos[2], targetPos[2], state.vel[2], state.velSmoothTime, state.linSpeed, dt);
            state.pos = [targetPos[0], targetPos[1], sz.value];
            state.vel = [0, 0, sz.vel];
        } else { // ToB or DescendB
            const delta: Vec3 = v.sub(targetPos, state.pos);
            const dist = v.len(delta);
            const dir = dist > 1e-6 ? v.scale(delta, 1 / dist) : [0, 0, 0];
            const stopRadius = state.margin + 0.01;

            if (state.phase === Phase.ToB) {
                // Straight-line path with speed easing, direction always towards target
                let speedMag = v.len(state.vel);
                const a = 1.0 - Math.exp(-dt / state.velSmoothTime);
                let goal = state.linSpeed;
                // decelerate inside radius derived from height
                const decelRadius = Math.max(50.0, state.height * 0.5);
                if (dist < decelRadius) goal = Math.max(state.minSpeed, state.linSpeed * (dist / decelRadius));
                // approach goal smoothly
                speedMag = speedMag + (goal - speedMag) * a;
                if (dist > stopRadius) speedMag = Math.max(state.minSpeed, speedMag);
                const step = Math.min(speedMag * dt, dist);
                state.pos = v.add(state.pos, v.scale(dir as Vec3, step));
                state.vel = v.scale(dir as Vec3, step > 0 && dt > 0 ? (step / dt) : 0);
            } else { // DescendB: constant-speed linear approach with exact stop
                const speedMag = Math.max(state.minSpeed, Math.min(state.linSpeed, dist / Math.max(dt, 1e-6)));
                const step = Math.min(speedMag * dt, dist);
                state.pos = v.add(state.pos, v.scale(dir as Vec3, step));
                state.vel = v.scale(dir as Vec3, step > 0 && dt > 0 ? (step / dt) : 0);
            }
        }

        // Angular smoothing (faster near arrival during DescendB so we reach POV angles in time)
        const maxAngSpeed = 720.0;
        let angSmooth = state.angSmoothTime;
        if (state.phase === Phase.DescendB) {
            const distForAng = v.len(v.sub(targetPos, state.pos));
            const blendRadius = Math.max(1.0, state.height * 0.5);
            const factor = Math.max(0.15, Math.min(1.0, distForAng / blendRadius));
            angSmooth = Math.max(0.06, state.angSmoothTime * factor);
        }
        const pr = smoothDampAngle1D(state.ang[0], targetAngles[0], state.angVel[0], angSmooth, maxAngSpeed, dt);
        const yr = smoothDampAngle1D(state.ang[1], targetAngles[1], state.angVel[1], angSmooth, maxAngSpeed, dt);
        const rr = smoothDampAngle1D(state.ang[2], targetAngles[2], state.angVel[2], angSmooth, maxAngSpeed, dt);
        state.ang = [pr.value, yr.value, rr.value];
        state.angVel = [pr.vel, yr.vel, rr.vel];

        // Arrival checks and phase progression
        if (state.phase === Phase.AscendA) {
            const dx = state.pos[0] - targetPos[0];
            const dy = state.pos[1] - targetPos[1];
            const dz = state.pos[2] - targetPos[2];
            const horiz = Math.sqrt(dx * dx + dy * dy);
            if (horiz <= state.margin && Math.abs(dz) <= state.margin) {
                beginPhase(Phase.HoldA);
            }
        } else if (state.phase === Phase.HoldA) {
            if ((mirv.getCurTime() - state.phaseStart) >= state.holdTime) {
                // Switch spectated player to destination so releasing later lands on them
                const outName = getNameForControllerIndex(state.toIdx);
                const qName = outName.includes(' ') ? `"${outName.replaceAll('"', '\\"')}"` : outName;
                mirv.exec('spec_mode 5');
                mirv.exec(`spec_player ${qName}`);
                // Capture and align yaw to world axes (nearest 90°)
                state.downYaw = snapYawToCardinal(state.ang[1]);
                beginPhase(Phase.ToB);
            }
        } else if (state.phase === Phase.ToB) {
            const dx = state.pos[0] - targetPos[0];
            const dy = state.pos[1] - targetPos[1];
            const dz = state.pos[2] - targetPos[2];
            const horiz = Math.sqrt(dx * dx + dy * dy);
            if (horiz <= state.margin && Math.abs(dz) <= state.margin) {
                beginPhase(Phase.HoldB);
            }
        } else if (state.phase === Phase.HoldB) {
            if ((mirv.getCurTime() - state.phaseStart) >= state.holdTime) {
                beginPhase(Phase.DescendB);
            }
        } else if (state.phase === Phase.DescendB) {
            const d = v.len(v.sub(state.pos, targetPos));
            if (d <= state.margin) {
                mirv.exec('mirv_input end');
                try { mirv.getMainCampath().enabled = false; } catch {}
                state.active = false; state.initialized = false; mirv.onCViewRenderSetupView = undefined;
                const doneName = getNameForControllerIndex(state.toIdx);
                mirv.message(`mirv_bird: done -> ${doneName}\n`);
            }
        }

        return { x: state.pos[0], y: state.pos[1], z: state.pos[2], rX: state.ang[0], rY: state.ang[1], rZ: state.ang[2] };
    }

    const fn: AdvancedfxConCommand.OnCallback = (args) => {
        const argc = args.argC();
        const cmd = args.argV(0);
        if (argc >= 2) {
            const sub = args.argV(1).toLowerCase();
            if (sub === 'goto') {
                if (argc >= 4) {
                    const toIdx = parseInt(args.argV(2));
                    const height = parseFloat(args.argV(3));
                    if (Number.isFinite(toIdx) && toIdx >= 0 && Number.isFinite(height) && height > 0) {
                        const fromIdx = getObservedControllerIndex();
                        if (fromIdx === null) {
                            mirv.warning('mirv_bird: could not detect current observed player (ensure you are in a player POV).\n');
                            return;
                        }
                        state.fromIdx = fromIdx;
                        state.toIdx = toIdx;
                        state.height = height;
                        // Optional tuning via extra args
                        state.linSpeed = argc >= 5 ? Math.max(50, parseFloat(args.argV(4)) || 500) : 500.0;
                        state.holdTime = argc >= 6 ? Math.max(0.0, parseFloat(args.argV(5)) || 1.0) : 1.0;
                        state.velSmoothTime = argc >= 7 ? Math.max(0.05, parseFloat(args.argV(6)) || 0.5) : 0.5;
                        state.angSmoothTime = argc >= 8 ? Math.max(0.05, parseFloat(args.argV(7)) || 0.25) : 0.25;
                        state.margin = argc >= 9 ? Math.max(0.5, parseFloat(args.argV(8)) || 5.0) : 5.0;
                        

                        state.active = true; state.initialized = false;
                        mirv.onCViewRenderSetupView = onView;
                        beginPhase(Phase.AscendA);

                        const fromName = getNameForControllerIndex(fromIdx);
                        const toName = getNameForControllerIndex(toIdx);
                        const qFrom = fromName.includes(' ') ? `"${fromName.replaceAll('"', '\\"')}"` : fromName;
                        mirv.exec('spec_mode 5');
                        mirv.exec(`spec_player ${qFrom}`);
                        mirv.message(`mirv_bird: from ${fromName} -> ${toName} height=${height.toFixed(1)} speed=${state.linSpeed.toFixed(0)} hold=${state.holdTime.toFixed(1)}s\n`);
                        return;
                    }
                }
                mirv.message(`${cmd} goto <controllerIndex> <height> [speed=500] [hold=1.0] [velSmooth=0.5] [angSmooth=0.25] [margin=5]\n`);
                return;
            }
            if (sub === 'stop') {
                state.active = false; state.initialized = false; mirv.onCViewRenderSetupView = undefined;
                mirv.message('mirv_bird: stopped\n');
                return;
            }
        }

        mirv.message(
            `${cmd} goto <controllerIndex> <height> [speed=500] [hold=1.0] [velSmooth=0.5] [angSmooth=0.25] [margin=5]\n` +
            `${cmd} stop\n`.dedent()
        );
    };

    // Register command mirv_bird
    // @ts-ignore
    (mirv as any)._mirv_bird = new AdvancedfxConCommand(fn);
    // @ts-ignore
    (mirv as any)._mirv_bird.register('mirv_bird', 'Birds-eye transition between players and back to POV.');
}
