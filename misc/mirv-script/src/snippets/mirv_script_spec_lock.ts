{
	String.prototype.dedent = function () {
		return this.split('\n')
			.map((l) => l.trim())
			.join('\n');
	};

	let idx: number | null = null;

	// @ts-ignore
	if (mirv._mirv_script_spec_lock !== undefined) mirv._mirv_script_spec_lock.unregister();
	// @ts-ignore
	mirv._mirv_script_spec_lock = new AdvancedfxConCommand((args) => {
		const argC = args.argC();
		const arg0 = args.argV(0);

		if (2 <= argC) {
			const arg1 = args.argV(1).toLowerCase();

			if (arg1 === 'list') {
				for (let i = 0; i < 64; i++) {
					const entity = mirv.getEntityFromIndex(i + 1);
					if (null !== entity && entity.isPlayerController()) {
						mirv.message(`${i + 1} : ${entity.getSanitizedPlayerName()}\n`);
					}
				}
				return;
			}

			idx = parseInt(arg1);
			if (isNaN(idx) || idx === 0) idx = null;

			mirv.onClientFrameStageNotify = (e) => {
				if (!e.isBefore && idx) {
					mirv.exec(`spec_player ${idx}`);
				}
			};

			return;
		}

		mirv.message(
			`Usage:
			${arg0} <i> - Lock spectating to player with index <i>.
			${arg0} 0 - To disable.
			${arg0} list - List players.
			Current value: ${idx ?? 'none'}
			`.dedent()
		);
	});

	// @ts-ignore
	mirv._mirv_script_spec_lock.register('mirv_script_spec_lock', '');
}
