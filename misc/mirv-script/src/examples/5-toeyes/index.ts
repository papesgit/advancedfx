{
    // Simple helper for usage text formatting like other snippets
    // eslint-disable-next-line no-extend-native
    // @ts-ignore
    String.prototype.dedent = function () {
        return this.split('\n').map((l: string) => l.trim()).join('\n');
    };

    // Unregister previous command if reloaded
    // @ts-ignore
    if ((mirv as any)._mirv_toeyes !== undefined) (mirv as any)._mirv_toeyes.unregister();

    type Vec3 = [number, number, number];

    const v = {
        add: (a: Vec3, b: Vec3): Vec3 => [a[0] + b[0], a[1] + b[1], a[2] + b[2]],
        sub: (a: Vec3, b: Vec3): Vec3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]],
        scale: (a: Vec3, s: number): Vec3 => [a[0] * s, a[1] * s, a[2] * s],
        len: (a: Vec3): number => Math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]),
        norm: (a: Vec3): Vec3 => {
            const l = v.len(a); if (l <= 1e-6) return [0, 0, 0]; return [a[0] / l, a[1] / l, a[2] / l];
        },
        dot: (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2],
    };

    function angleNormalize(a: number): number { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
    function angleDelta(a: number, b: number): number { return angleNormalize(a - b); }

    // Constant-speed intercept (returns intercept point and time if solvable)
    function computeInterceptPoint(chaserPos: Vec3, targetPos: Vec3, targetVel: Vec3, chaserSpeed: number): { p: Vec3; t: number } | null {
        const r: Vec3 = v.sub(targetPos, chaserPos);
        const vv = v.dot(targetVel, targetVel);
        const s2 = chaserSpeed * chaserSpeed;
        const a = vv - s2;
        const b = 2 * v.dot(r, targetVel);
        const c = v.dot(r, r);
        let t = -1;
        const eps = 1e-6;
        if (Math.abs(a) < eps) {
            if (Math.abs(b) > eps) t = -c / b;
        } else {
            const disc = b * b - 4 * a * c;
            if (disc >= 0) {
                const sd = Math.sqrt(disc);
                const t1 = (-b - sd) / (2 * a);
                const t2 = (-b + sd) / (2 * a);
                if (t1 > eps && t2 > eps) t = Math.min(t1, t2);
                else if (t1 > eps) t = t1; else if (t2 > eps) t = t2;
            }
        }
        if (!(t > 0) || !isFinite(t)) return null;
        return { p: v.add(targetPos, v.scale(targetVel, t as number)), t };
    }

    const state = {
        active: false,
        name: '',
        resolvedName: '',
        controllerIndex: -1 as number,
        pawnHandle: 0 as number,
        pendingClosest: false,
        pendingFov: false,
        fovDeg: 60.0,
        // Kinematics
        pos: [0, 0, 0] as Vec3,
        vel: [0, 0, 0] as Vec3,
        ang: [0, 0, 0] as Vec3,
        lastTgtOrg: [0, 0, 0] as Vec3,
        angVel: [0, 0, 0] as Vec3,
        // Smoothing
        velSmoothTime: 1.0, // s (match C++)
        angSmoothTime: 1.5, // s (match C++)
        tgtVelSmoothTime: 0.20, // s
        smoothedTgtVel: [0, 0, 0] as Vec3,
        hasSmoothedTgtVel: false,
        // Speeds
        targetSpeed: 300.0, // units/s baseline
        // Bookkeeping
        initialized: false,
        startTime: 0,
        // Config
        margin: 5.0,
    };

    // name-based start removed: resolveByNameOnce()

    function getEyesByCachedTarget(): { org: Vec3; ang: Vec3 } | null {
        if (state.pawnHandle && mirv.isHandleValid(state.pawnHandle)) {
            const pawnIdx = mirv.getHandleEntryIndex(state.pawnHandle);
            const pawn = mirv.getEntityFromIndex(pawnIdx);
            if (pawn && pawn.isPlayerPawn()) {
                const orgArr = pawn.getRenderEyeOrigin();
                const angArr = pawn.getRenderEyeAngles();
                return { org: [orgArr[0], orgArr[1], orgArr[2]], ang: [angArr[0], angArr[1], angArr[2]] };
            }
        }
        if (state.controllerIndex >= 0) {
            const ent = mirv.getEntityFromIndex(state.controllerIndex);
            if (ent && ent.isPlayerController()) {
                try { state.pawnHandle = ent.getPlayerPawnHandle(); } catch { state.pawnHandle = 0; }
                if (state.pawnHandle && mirv.isHandleValid(state.pawnHandle)) {
                    const pawnIdx = mirv.getHandleEntryIndex(state.pawnHandle);
                    const pawn = mirv.getEntityFromIndex(pawnIdx);
                    if (pawn && pawn.isPlayerPawn()) {
                        const orgArr = pawn.getRenderEyeOrigin();
                        const angArr = pawn.getRenderEyeAngles();
                        return { org: [orgArr[0], orgArr[1], orgArr[2]], ang: [angArr[0], angArr[1], angArr[2]] };
                    }
                }
            }
        }
        return null;
    }

    function resolveClosestInView(e: mirv.OnCViewRenderSetupViewArgs): boolean {
        const hi = mirv.getHighestEntityIndex();
        // Strictly nearest pawn; skip too-near self observer.
        const minSelf2 = 5.0 * 5.0;
        let bestIdx = -1; let bestD2 = Number.POSITIVE_INFINITY;
        let nearIdx = -1; let nearD2 = Number.POSITIVE_INFINITY;
        for (let i = 0; i <= hi; i++) {
            const ent = mirv.getEntityFromIndex(i);
            if (!ent) continue;
            try {
                if (!ent.isPlayerPawn()) continue;
                const org = ent.getRenderEyeOrigin();
                const dx = org[0] - e.currentView.x;
                const dy = org[1] - e.currentView.y;
                const dz = org[2] - e.currentView.z;
                const d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < minSelf2) { if (d2 < nearD2) { nearD2 = d2; nearIdx = i; } continue; }
                if (d2 < bestD2) { bestD2 = d2; bestIdx = i; }
            } catch { /* ignore */ }
        }
        const chosenIdx = (bestIdx >= 0) ? bestIdx : nearIdx;
        if (chosenIdx >= 0) {
            const pawn = mirv.getEntityFromIndex(chosenIdx);
            if (pawn && pawn.isPlayerPawn()) {
                let ctrlIdx = -1;
                try {
                    const h = pawn.getPlayerControllerHandle();
                    if (h && mirv.isHandleValid(h)) ctrlIdx = mirv.getHandleEntryIndex(h);
                } catch { /* ignore */ }
                if (ctrlIdx < 0) {
                    for (let i = 0; i <= hi; i++) {
                        const c = mirv.getEntityFromIndex(i);
                        if (!c) continue;
                        try {
                            if (!c.isPlayerController()) continue;
                            const h = c.getPlayerPawnHandle();
                            if (h && mirv.isHandleValid(h) && mirv.getHandleEntryIndex(h) === chosenIdx) { ctrlIdx = i; break; }
                        } catch { /* ignore */ }
                    }
                }
                if (ctrlIdx >= 0) {
                    state.controllerIndex = ctrlIdx;
                    try { state.pawnHandle = pawn.getPlayerPawnHandle(); } catch { state.pawnHandle = 0; }
                    try { const ctrl = mirv.getEntityFromIndex(ctrlIdx); const s = ctrl?.getSanitizedPlayerName(); if (s) state.resolvedName = s; } catch {}
                    state.pendingClosest = false;
                    state.active = true; state.initialized = false;
                    return true;
                }
            }
        }
        return false;
    }

    // name-based spectator resolution removed

    function resolveFovInView(e: mirv.OnCViewRenderSetupViewArgs, fovDeg: number): boolean {
        const hi = mirv.getHighestEntityIndex();

        // Camera forward from pitch/yaw (degrees)
        const deg2rad = (d: number) => d * Math.PI / 180.0;
        const pitch = deg2rad(e.currentView.rX);
        const yaw = deg2rad(e.currentView.rY);
        const cp = Math.cos(pitch), sp = Math.sin(pitch);
        const cy = Math.cos(yaw),   sy = Math.sin(yaw);
        const camFwd: Vec3 = [cp * cy, cp * sy, -sp];

        // Prefer smallest angular offset within fovDeg cone; fallback to nearest
        const minCos = Math.cos(Math.max(1.0, Math.min(170.0, fovDeg)) * Math.PI / 180.0);
        let bestIdx = -1; let bestCos = -2; let bestD2 = Number.POSITIVE_INFINITY;
        let nearIdx = -1; let nearD2 = Number.POSITIVE_INFINITY;

        for (let i = 0; i <= hi; i++) {
            const ent = mirv.getEntityFromIndex(i);
            if (!ent) continue;
            try {
                if (!ent.isPlayerPawn()) continue;
                const org = ent.getRenderEyeOrigin();
                const dx = org[0] - e.currentView.x;
                const dy = org[1] - e.currentView.y;
                const dz = org[2] - e.currentView.z;
                const d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < 1e-6) continue;
                if (d2 < nearD2) { nearD2 = d2; nearIdx = i; }

                const invLen = 1.0 / Math.sqrt(d2);
                const dir: Vec3 = [dx * invLen, dy * invLen, dz * invLen];
                const c = camFwd[0]*dir[0] + camFwd[1]*dir[1] + camFwd[2]*dir[2];
                if (c >= minCos) {
                    if (c > bestCos || (Math.abs(c - bestCos) < 1e-6 && d2 < bestD2)) {
                        bestCos = c; bestIdx = i; bestD2 = d2;
                    }
                }
            } catch { /* ignore */ }
        }

        const chosenIdx = bestIdx >= 0 ? bestIdx : nearIdx;
        if (chosenIdx >= 0) {
            const pawn = mirv.getEntityFromIndex(chosenIdx);
            if (pawn && pawn.isPlayerPawn()) {
                let ctrlIdx = -1;
                try {
                    const h = pawn.getPlayerControllerHandle();
                    if (h && mirv.isHandleValid(h)) ctrlIdx = mirv.getHandleEntryIndex(h);
                } catch { /* ignore */ }
                if (ctrlIdx < 0) {
                    for (let i = 0; i <= hi; i++) {
                        const c = mirv.getEntityFromIndex(i);
                        if (!c) continue;
                        try {
                            if (!c.isPlayerController()) continue;
                            const h = c.getPlayerPawnHandle();
                            if (h && mirv.isHandleValid(h) && mirv.getHandleEntryIndex(h) === chosenIdx) { ctrlIdx = i; break; }
                        } catch { /* ignore */ }
                    }
                }
                if (ctrlIdx >= 0) {
                    state.controllerIndex = ctrlIdx;
                    try { state.pawnHandle = pawn.getPlayerPawnHandle(); } catch { state.pawnHandle = 0; }
                    try { const ctrl = mirv.getEntityFromIndex(ctrlIdx); const s = ctrl?.getSanitizedPlayerName(); if (s) state.resolvedName = s; } catch {}
                    state.pendingFov = false;
                    state.active = true; state.initialized = false;
                    return true;
                }
            }
        }
        return false;
    }

    // Critically damped smoothing, mirrors C++ SmoothDamp1D
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
        // prevent overshoot
        if (((originalTo - current) > 0.0) === (output > originalTo)) {
            output = originalTo;
            // recompute vel to be consistent
            // note: this is a copy from Unity-like implementation, small discrepancies OK
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

    function onView(e: mirv.OnCViewRenderSetupViewArgs): mirv.OnCViewRenderSetupViewSet | undefined {
        if (state.pendingClosest && !state.active) {
            if (!resolveClosestInView(e)) {
                // could not resolve this frame, keep waiting a bit
                if ((mirv.getCurTime() - state.startTime) > 3.0) {
                    mirv.warning('mirv_toeyes: closest target not found, aborting.\n');
                    state.pendingClosest = false; mirv.onCViewRenderSetupView = undefined; return;
                }
                return;
            }
            const outName = (state.resolvedName && state.resolvedName.length)
                ? state.resolvedName
                : (state.controllerIndex >= 0 ? `#${state.controllerIndex}` : state.name);
            const qName = outName.includes(' ') ? `"${outName.replaceAll('"', '\\"')}"` : outName;
            mirv.exec('spec_mode 5');
            mirv.exec(`spec_player ${qName}`);
            mirv.message(`mirv_toeyes: started (closest) for ${state.resolvedName || outName} (speed=${state.targetSpeed.toFixed(1)})\n`);
        }
        if (state.pendingFov && !state.active) {
            if (!resolveFovInView(e, state.fovDeg)) {
                if ((mirv.getCurTime() - state.startTime) > 3.0) {
                    mirv.warning('mirv_toeyes: fov target not found, aborting.\n');
                    state.pendingFov = false; mirv.onCViewRenderSetupView = undefined; return;
                }
                return;
            }
            const outName2 = (state.resolvedName && state.resolvedName.length)
                ? state.resolvedName
                : (state.controllerIndex >= 0 ? `#${state.controllerIndex}` : state.name);
            const qName2 = outName2.includes(' ') ? `"${outName2.replaceAll('"', '\\"')}"` : outName2;
            mirv.exec('spec_mode 5');
            mirv.exec(`spec_player ${qName2}`);
            mirv.message(`mirv_toeyes: started (fov=${state.fovDeg.toFixed(0)}) for ${state.resolvedName || outName2} (speed=${state.targetSpeed.toFixed(1)})\n`);
        }
        // name-based spectator resolution removed
        if (!state.active) return;

        const tgt = getEyesByCachedTarget();
        if (!tgt) {
            if ((mirv.getCurTime() - state.startTime) > 3.0) {
                mirv.warning(`mirv_toeyes: target not available, aborting.\n`);
                state.active = false; state.initialized = false; mirv.onCViewRenderSetupView = undefined; return;
            }
            return; // keep trying next frame
        }

        // Initialize on first target sample using current view
        if (!state.initialized) {
            state.pos = [e.currentView.x, e.currentView.y, e.currentView.z];
            state.ang = [e.currentView.rX, e.currentView.rY, e.currentView.rZ];
            // Prefer seeding from campath derivative if available, fallback to last-frame deltas
            const dt0 = e.absTime > 1e-6 ? e.absTime : 1 / 64.0;
            let seeded = false;
            try {
                const cp = mirv.getMainCampath();
                if (cp && cp.enabled && cp.canEval) {
                    let t = e.curTime - cp.offset;
                    if (cp.hold) {
                        if (typeof cp.lowerBound === 'number' && t < cp.lowerBound) t = cp.lowerBound;
                        if (typeof cp.upperBound === 'number' && t > cp.upperBound) t = cp.upperBound;
                    }
                    const v1 = cp.eval(t);
                    const v0 = cp.eval(t - dt0);
                    if (v1 && v0) {
                        // Linear velocity from campath
                        state.vel = [
                            (v1.pos.x - v0.pos.x) / dt0,
                            (v1.pos.y - v0.pos.y) / dt0,
                            (v1.pos.z - v0.pos.z) / dt0,
                        ];
                        // Angular velocity from campath
                        const a1 = v1.rot.toQREulerAngles().toQEulerAngles();
                        const a0 = v0.rot.toQREulerAngles().toQEulerAngles();
                        state.angVel = [
                            angleDelta(a1.pitch, a0.pitch) / dt0,
                            angleDelta(a1.yaw,   a0.yaw)   / dt0,
                            angleDelta(a1.roll,  a0.roll)  / dt0,
                        ];
                        state.ang = [a1.pitch, a1.yaw, a1.roll];
                        seeded = true;
                    }
                }
            } catch { /* ignore */ }
            if (!seeded) {
                // Fallback seeding from last frame camera delta
                const dv: Vec3 = [e.currentView.x - e.lastView.x, e.currentView.y - e.lastView.y, e.currentView.z - e.lastView.z];
                state.vel = v.scale(dv, 1 / dt0);
                const dPitch = angleDelta(e.currentView.rX, e.lastView.rX);
                const dYaw = angleDelta(e.currentView.rY, e.lastView.rY);
                const dRoll = angleDelta(e.currentView.rZ, e.lastView.rZ);
                state.angVel = [dPitch / dt0, dYaw / dt0, dRoll / dt0];
                state.ang = [e.currentView.rX, e.currentView.rY, e.currentView.rZ];
            }
            state.lastTgtOrg = tgt.org;
            state.smoothedTgtVel = [0, 0, 0];
            state.hasSmoothedTgtVel = false;
            state.initialized = true;
        }

        const dt = Math.max(1 / 128.0, Math.min(e.absTime || 0.0, 0.25));

        // Target velocity (EMA)
        const rawTgtVel = v.scale(v.sub(tgt.org, state.lastTgtOrg), 1 / dt);
        state.lastTgtOrg = tgt.org;
        const aTgt = 1.0 - Math.exp(-dt / state.tgtVelSmoothTime);
        if (!state.hasSmoothedTgtVel) { state.smoothedTgtVel = rawTgtVel; state.hasSmoothedTgtVel = true; }
        else {
            state.smoothedTgtVel = v.add(state.smoothedTgtVel, v.scale(v.sub(rawTgtVel, state.smoothedTgtVel), aTgt));
        }

        // Intercept point with dynamic near-target boost
        const toTgt = v.sub(tgt.org, state.pos);
        const dist = v.len(toTgt);
        let speedBoost = 1.0;
        if (dist < 120.0) speedBoost = 1.0 + (120.0 - dist) / 120.0 * 0.5; // up to +50%
        const chaseSpeed = state.targetSpeed * speedBoost;
        let pred = computeInterceptPoint(state.pos, tgt.org, state.smoothedTgtVel, chaseSpeed)?.p;
        if (!pred) {
            // fallback: simple lead
            const speed = Math.max(1.0, v.len(state.vel));
            const lead = Math.max(0.05, Math.min(dist / speed, 0.5));
            pred = v.add(tgt.org, v.scale(state.smoothedTgtVel, lead));
        }

        // Desired heading: intercept + lateral correction + base direction
        const toPred = v.sub(pred, state.pos);
        const interceptDir = v.norm(toPred);
        const baseDir = v.norm(toTgt);
        const tgtSpeedMag = v.len(state.smoothedTgtVel);
        const fwd: Vec3 = tgtSpeedMag > 10 ? v.scale(state.smoothedTgtVel, 1 / tgtSpeedMag) : [0, 0, 0];
        let lateral = toTgt as Vec3;
        if (tgtSpeedMag > 10) {
            const along = v.dot(toTgt, fwd);
            lateral = v.sub(toTgt, v.scale(fwd, along));
        }
        const lateralMag = v.len(lateral);
        const lateralDir = v.norm(lateral);
        let wLat = 0.5 * (lateralMag / (lateralMag + Math.abs(tgtSpeedMag) + 1e-3));
        let wBase = 0.3 * (dist > 1e-3 ? (lateralMag / dist) : 0.0);
        const closeFactor = dist >= 400 ? 0.0 : (1.0 - (dist / 400.0));
        wLat += closeFactor * 0.25; wBase += closeFactor * 0.15;
        let wInt = 1.0 - (wLat + wBase); if (wInt < 0.15) wInt = 0.15;
        let dir = v.add(v.add(v.scale(interceptDir, wInt), v.scale(lateralDir, wLat)), v.scale(baseDir, wBase));
        dir = v.norm(dir);

        // Velocity easing
        const velSmooth = Math.max(0.12, state.velSmoothTime * (dist > 400 ? 1.0 : (dist / 400.0)));
        const aVel = 1.0 - Math.exp(-dt / velSmooth);
        const desiredVel = v.scale(dir, chaseSpeed);
        state.vel = v.add(state.vel, v.scale(v.sub(desiredVel, state.vel), aVel));
        const speedNow = v.len(state.vel);
        if (speedNow > chaseSpeed) state.vel = v.scale(state.vel, chaseSpeed / speedNow);
        state.pos = v.add(state.pos, v.scale(state.vel, dt));

        // Angles: SmoothDampAngle per axis, like C++
        const maxAngSpeed = 420.0; // deg/s
        const pitchR = smoothDampAngle1D(state.ang[0], tgt.ang[0], state.angVel[0], state.angSmoothTime, maxAngSpeed, dt);
        state.ang[0] = pitchR.value; state.angVel[0] = pitchR.vel;
        const yawR   = smoothDampAngle1D(state.ang[1], tgt.ang[1], state.angVel[1], state.angSmoothTime, maxAngSpeed, dt);
        state.ang[1] = yawR.value;   state.angVel[1] = yawR.vel;
        const rollR  = smoothDampAngle1D(state.ang[2], tgt.ang[2], state.angVel[2], state.angSmoothTime, maxAngSpeed, dt);
        state.ang[2] = rollR.value;  state.angVel[2] = rollR.vel;

        // Override view
        const Tx = state.pos[0], Ty = state.pos[1], Tz = state.pos[2];
        const Rx = state.ang[0], Ry = state.ang[1], Rz = state.ang[2];

        // Arrival: switch when about to cross target within a small margin
        const step = v.len(state.vel) * dt;
        if (((step >= (dist - state.margin)) && dist <= 50.0) || dist <= 5.0) {
            const outName = state.resolvedName || state.name;
            const qName = outName.includes(' ') ? `"${outName.replaceAll('"', '\\"')}"` : outName;
            mirv.exec('mirv_input end');
            try { mirv.getMainCampath().enabled = false; } catch {}
            state.active = false; state.initialized = false; mirv.onCViewRenderSetupView = undefined;
            mirv.message(`mirv_toeyes: done -> ${outName}\n`);
        }

        return { x: Tx, y: Ty, z: Tz, rX: Rx, rY: Ry, rZ: Rz };
    }

    const fn: AdvancedfxConCommand.OnCallback = (args) => {
        const argc = args.argC();
        const cmd = args.argV(0);
        if (argc >= 2) {
            const sub = args.argV(1).toLowerCase();
            // 'start <playername>' removed
            if (sub === 'start_index') {
                if (argc >= 3) {
                    const idx = parseInt(args.argV(2));
                    if (Number.isFinite(idx) && idx >= 0) {
                        state.controllerIndex = idx; state.pawnHandle = 0; state.name = `#${idx}`; state.resolvedName = '';
                        // Optional: fetch sanitized name once for feedback
                        try { const ent = mirv.getEntityFromIndex(idx); if (ent && ent.isPlayerController()) { const s = ent.getSanitizedPlayerName(); if (s) state.resolvedName = s; } } catch {}
                        state.targetSpeed = argc >= 4 ? Math.max(50, parseFloat(args.argV(3)) || 300) : 300;
                        state.velSmoothTime = argc >= 5 ? Math.max(0.05, parseFloat(args.argV(4)) || 0.55) : state.velSmoothTime;
                        state.angSmoothTime = argc >= 6 ? Math.max(0.05, parseFloat(args.argV(5)) || 0.16) : state.angSmoothTime;
                        state.margin = argc >= 7 ? Math.max(0.0, parseFloat(args.argV(6)) || 5.0) : state.margin;
                        state.active = true; state.initialized = false; state.startTime = mirv.getCurTime();
                        mirv.onCViewRenderSetupView = onView;
                        const qName = state.name.includes(' ') ? `"${state.name.replaceAll('"', '\\"')}"` : state.name;
                        mirv.exec('spec_mode 5');
                        mirv.exec(`spec_player ${state.controllerIndex}`);
                        mirv.message(`mirv_toeyes: started for ${state.resolvedName || state.name} (speed=${state.targetSpeed.toFixed(1)})\n`);
                        return;
                    }
                }
                mirv.message(`${cmd} start_index <controllerIndex> [speed=300] [velSmooth=0.55] [angSmooth=0.16] [margin=5]\n`);
                return;
            }
            if (sub === 'start_closest') {
                state.targetSpeed = argc >= 3 ? Math.max(50, parseFloat(args.argV(2)) || 300) : 300;
                state.velSmoothTime = argc >= 4 ? Math.max(0.05, parseFloat(args.argV(3)) || 0.55) : state.velSmoothTime;
                state.angSmoothTime = argc >= 5 ? Math.max(0.05, parseFloat(args.argV(4)) || 0.16) : state.angSmoothTime;
                state.margin = argc >= 6 ? Math.max(0.0, parseFloat(args.argV(5)) || 5.0) : state.margin;
                state.controllerIndex = -1; state.pawnHandle = 0; state.resolvedName = ''; state.name = '[closest]';
                state.pendingClosest = true; state.active = false; state.initialized = false; state.startTime = mirv.getCurTime();
                mirv.onCViewRenderSetupView = onView;
                mirv.message('mirv_toeyes: resolving closest player ...\n');
                return;
            }
            if (sub === 'start_fov') {
                const fov = argc >= 3 ? Math.max(1, Math.min(170, parseFloat(args.argV(2)) || 60)) : 60;
                state.targetSpeed = argc >= 4 ? Math.max(50, parseFloat(args.argV(3)) || 300) : 300;
                state.velSmoothTime = argc >= 5 ? Math.max(0.05, parseFloat(args.argV(4)) || 0.55) : state.velSmoothTime;
                state.angSmoothTime = argc >= 6 ? Math.max(0.05, parseFloat(args.argV(5)) || 0.16) : state.angSmoothTime;
                state.margin = argc >= 7 ? Math.max(0.0, parseFloat(args.argV(6)) || 5.0) : state.margin;
                state.controllerIndex = -1; state.pawnHandle = 0; state.resolvedName = ''; state.name = `[fov=${fov}]`;
                state.fovDeg = fov; state.pendingFov = true; state.active = false; state.initialized = false; state.startTime = mirv.getCurTime();
                mirv.onCViewRenderSetupView = onView;
                mirv.message(`mirv_toeyes: resolving target in ${fov}° FOV ...\n`);
                return;
            }
            if (sub === 'stop') {
                state.active = false; state.initialized = false; mirv.onCViewRenderSetupView = undefined;
                mirv.message('mirv_toeyes: stopped\n');
                return;
            }
        }
        mirv.message(
            `${cmd} start_index <controllerIndex> [speed=300] [velSmooth=0.55] [angSmooth=0.16] [margin=5]\n` +
            `${cmd} start_closest [speed=300] [velSmooth=0.55] [angSmooth=0.16] [margin=5]\n` +
            `${cmd} start_fov [fov=60] [speed=300] [velSmooth=0.55] [angSmooth=0.16] [margin=5]\n` +
            `${cmd} stop\n`.dedent()
        );
    };

    // Register command mirv_toeyes
    // @ts-ignore
    (mirv as any)._mirv_toeyes = new AdvancedfxConCommand(fn);
    // @ts-ignore
    (mirv as any)._mirv_toeyes.register('mirv_toeyes', 'Chase camera to player POV (toeyes).');
}
