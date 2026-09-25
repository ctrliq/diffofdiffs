/* SPDX-License-Identifier: GPL-2.0-only */
(() => {
	'use strict';
	const root = document.documentElement;
	const report = document.querySelector('#comparison');
	const sections = [...report.querySelectorAll('section')];
	const fileSelect = document.querySelector('#file-select');
	const position = document.querySelector('#change-position');
	const status = document.querySelector('#copy-status');
	const plainReport =
		document.querySelector('#plain-report').content.textContent;
	const selectors = [
		['theme-select', 'theme'],
		['layout-select', 'layout'],
		['highlight-select', 'highlight'],
		['font-size', 'textSize']
	];
	const checkboxes = [
		['shared-yellow', 'shared'],
		['gap-hatching', 'gaps'],
		['wrap-lines', 'wrap']
	];
	const viewUpdates = [];
	let groups = [];
	let groupIndex = -1;

	function bindViewControl(id, apply) {
		const control = document.getElementById(id);
		const update = () => apply(control);

		control.addEventListener('change', update);
		viewUpdates.push(update);
		return control;
	}

	function applyViewControls() {
		for (const update of viewUpdates)
			update();
	}

	function downloadText(filename, text) {
		const blob = new Blob([text], {type: 'text/plain;charset=utf-8'});
		const url = URL.createObjectURL(blob);
		const link = document.createElement('a');

		link.href = url;
		link.download = filename;
		link.click();
		setTimeout(() => URL.revokeObjectURL(url), 1000);
	}

	function updateNavigation() {
		const shown = sections.filter(section => !section.hidden);
		groups = shown.flatMap(section => {
			return [...section.querySelectorAll('[data-change-start]')];
		});
		groupIndex = -1;
		for (const row of report.querySelectorAll('.current-change'))
			row.classList.remove('current-change');
		position.textContent = `${groups.length} difference groups`;
		for (const id of ['previous-change', 'next-change'])
			document.getElementById(id).disabled = !groups.length;
	}

	function addSectionFiles(section) {
		const name = section.id === 'delta' ? 'Delta' : 'Context';
		for (const file of section.querySelectorAll('.file-block')) {
			const heading = file.querySelector('.file-heading');
			const paths = Array.from(heading.children,
				span => span.textContent);
			const path = paths[0] === paths[1] ? paths[0] :
				paths.filter(Boolean).join(' → ');
			fileSelect.add(new Option(`${name} · ${path}`, file.id));
		}
	}

	function setView(value) {
		fileSelect.replaceChildren(new Option('Choose a file', ''));
		for (const section of sections) {
			section.hidden = value !== 'all' && section.id !== value;
			if (!section.hidden)
				addSectionFiles(section);
		}
		updateNavigation();
	}

	function moveChange(step) {
		let row;

		if (!groups.length)
			return;

		groups[groupIndex]?.classList.remove('current-change');
		groupIndex = groupIndex < 0 ?
			(step < 0 ? groups.length - 1 : 0) :
			(groupIndex + step + groups.length) % groups.length;
		row = groups[groupIndex];
		row.classList.add('current-change');
		row.scrollIntoView({block: 'center', inline: 'nearest'});
		position.textContent = `${groupIndex + 1} of ${groups.length} difference groups`;
	}

	function createIndentationLabel(run) {
		const shift = Number(run[0].dataset.indentShift);
		const label = document.createElement('div');
		const name = document.createElement('strong');
		const detail = document.createElement('span');
		const lines = run.length === 1 ? 'line' : 'lines';
		const sign = shift > 0 ? '+' : '';
		const change = shift ?
			`right side ${sign}${shift} columns` :
			'tabs / spaces changed';

		label.className = 'indent-label';
		name.textContent = 'Indentation only';
		detail.textContent = `${run.length} ${lines} · ${change}`;
		label.title = 'Source text matches exactly after removing only leading spaces and tabs.';
		label.append(name, detail);
		return label;
	}

	/*
	 * Group adjacent rows only when their exact indentation checks found
	 * the same shift.
	 */
	report.querySelectorAll('.hunk').forEach(hunk => {
		let run = [];
		function finishRun() {
			if (!run.length)
				return;

			run[0].before(createIndentationLabel(run));
			run = [];
		}
		[...hunk.children].forEach(row => {
			const shift = row.dataset.indentShift;

			if (!row.hasAttribute('data-indent-shift')) {
				finishRun();
				return;
			}
			if (run.length && run[0].dataset.indentShift !== shift)
				finishRun();
			run.push(row);
		});
		finishRun();
	});

	/* Embedded defaults also style the report when JavaScript is disabled */
	for (const [id, property] of selectors) {
		const select = bindViewControl(id, control => {
			root.dataset[property] = control.value;
		});

		select.value = root.dataset[property];
	}
	for (const [id, property] of checkboxes) {
		const checkbox = bindViewControl(id, control => {
			root.dataset[property] = String(control.checked);
		});

		checkbox.checked = root.dataset[property] === 'true';
	}
	bindViewControl('section-select', control => setView(control.value));
	fileSelect.addEventListener('change', () => {
		document.getElementById(fileSelect.value)?.scrollIntoView({
			block: 'start', inline: 'nearest'
		});
	});
	document.querySelector('#previous-change')
		.addEventListener('click', () => moveChange(-1));
	document.querySelector('#next-change')
		.addEventListener('click', () => moveChange(1));
	document.querySelector('#copy-report')
		.addEventListener('click', async () => {
			try {
				await navigator.clipboard.writeText(
					plainReport);
				status.textContent = 'Complete report copied';
			} catch {
				status.textContent = 'Clipboard unavailable here. Use Save text instead.';
			}
		});
	document.querySelector('#download-report')
		.addEventListener('click', () => {
			downloadText('diffofdiffs.txt', plainReport);
		});

	/*
	 * Browsers can restore controls after this script runs without sending
	 * change events. Apply the restored state when the document is shown.
	 */
	window.addEventListener('pageshow', applyViewControls);
	applyViewControls();
	root.dataset.interactive = '';
})();
