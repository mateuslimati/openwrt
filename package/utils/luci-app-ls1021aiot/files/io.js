'use strict';
'require view';
'require poll';
'require rpc';
'require ui';
'require ls1021aiot.common as common';

/*
 * The digital I/Os: the 16 lines of the PCA9555 (U2) and, with CPLD image
 * 3.0, the 28 of the CPLD (U33). For each, the mode (input or output), the
 * level it reads as an input and the level to drive as an output.
 * IO0_0..IO0_7 of the PCA9555 and CPLD_EXPAND0-5 are on J501 (PORT 1 on
 * the rear panel); IO1_0..IO1_7 of the PCA9555 are wired to GPIO_EXPAND8-15
 * of the CPLD. GPIO3_15..27 of the LS1021A are on J502 (PORT 2), at 2.5 V.
 */

var callIo = rpc.declare({
	object: 'ls1021aiot',
	method: 'io',
	expect: { lines: [] }
});

var callSetIo = rpc.declare({
	object: 'ls1021aiot',
	method: 'set_io',
	params: [ 'line', 'dir', 'value' ]
});

var callLoopback = rpc.declare({
	object: 'ls1021aiot',
	method: 'loopback'
});

/* line -> where it goes; 0-15 PCA9555, 16-43 CPLD, 44-56 LS1021A GPIO3_15-27 */
var WHERE = [
	'J501.3', 'J501.5', 'J501.9', 'J501.11', 'J501.13', 'J501.17', 'J501.19', 'J501.18',
	'CPLD GPIO_EXPAND8', 'CPLD GPIO_EXPAND9', 'CPLD GPIO_EXPAND10', 'CPLD GPIO_EXPAND11',
	'CPLD GPIO_EXPAND12', 'CPLD GPIO_EXPAND13', 'CPLD GPIO_EXPAND14', 'CPLD GPIO_EXPAND15',
	'J501.2', 'J501.6', 'J501.8', 'J501.10', 'J501.14', 'J501.16',
	'J17.3 (Arduino D2)', 'J17.4 (Arduino D3)', 'J17.5 (Arduino D4)', 'J17.6 (Arduino D5)',
	'J17.7 (Arduino D6)', 'J17.8 (Arduino D7)',
	'PCA9555 IO1_0', 'PCA9555 IO1_1', 'PCA9555 IO1_2', 'PCA9555 IO1_3',
	'PCA9555 IO1_4', 'PCA9555 IO1_5', 'PCA9555 IO1_6', 'PCA9555 IO1_7',
	'TP9', 'TP10', 'TP11', 'TP12', 'TP13', 'TP14', 'TP15', 'TP16',
	'J502.19', 'J502.16', 'J502.11', 'J502.9', 'J502.6', 'J502.8', 'J502.10',
	'J502.17', 'J502.18', 'J502.5', 'J502.3', 'J502.13', 'J502.14'
];
var CPLD_FIRST = 16, CPU_FIRST = 44;
/* GPIO_EXPAND15 is an input on the CPLD */
var IN_ONLY = { 35: true };

/* connector pin -> what is on it: an I/O line (number) or a name */
var J501 = {
	1: '3,3 V', 2: 16, 3: 0, 4: '5 V', 5: 1, 6: 17, 7: 'GND', 8: 18, 9: 2, 10: 19,
	11: 3, 12: 'GND', 13: 4, 14: 20, 15: 'GND', 16: 21, 17: 5, 18: 7, 19: 6, 20: 'GND'
};
var J502 = {
	1: '2,5 V', 2: '5 V', 3: 54, 4: 'GND', 5: 53, 6: 48, 7: 'GND', 8: 49, 9: 47, 10: 50,
	11: 46, 12: 'GND', 13: 55, 14: 56, 15: 'GND', 16: 45, 17: 51, 18: 52, 19: 44, 20: 'GND'
};

function name(i) {
	if (i < 8)
		return 'IO0_' + i;
	if (i < 16)
		return 'IO1_' + (i - 8);
	if (i >= CPU_FIRST)
		return 'GPIO3_' + (i - CPU_FIRST + 15);
	i -= CPLD_FIRST;
	if (i < 6)
		return 'CPLD' + i;
	if (i < 12)
		return 'D' + (i - 4);
	if (i < 20)
		return 'GPX' + (i - 4);
	return 'TP' + (i - 11);
}

return view.extend({
	load: function() {
		return L.resolveDefault(callIo(), []);
	},

	handleSet: function(line, dir, value) {
		return callSetIo(line, dir, value).then(L.bind(function(res) {
			if (!res || !res.ok)
				ui.addNotification(null, E('p', 'Não foi possível mudar a linha ' + name(line) + '.'), 'error');
			return this.refresh();
		}, this));
	},

	refresh: function() {
		return L.resolveDefault(callIo(), []).then(L.bind(function(lines) {
			this.lines = lines;
			this.table.replaceChildren(this.renderTable(lines));
			this.header.replaceChildren(this.renderHeader(lines, J501, 'J501'));
			this.header2.replaceChildren(this.renderHeader(lines, J502, 'J502'));
			this.stamp.textContent = 'atualizado às ' + new Date().toLocaleTimeString();
		}, this));
	},

	handleLoopback: function() {
		this.loop.replaceChildren(E('p', { 'class': 'spinning' }, 'Testando as 8 ligações nos dois sentidos (uns 7 s)...'));
		return callLoopback().then(L.bind(function(r) {
			this.loop.replaceChildren(this.renderLoopback(r));
			return this.refresh();
		}, this));
	},

	renderLoopback: function(r) {
		if (!r || !r.ok)
			return E('p', { 'class': 'ls-muted' }, 'O teste precisa das E/S da CPLD (imagem 3.0).');

		var row = function(dir, label) {
			return E('tr', { 'class': 'tr' }, [ E('td', { 'class': 'td' }, label) ].concat([ 0, 1, 2, 3, 4, 5, 6, 7 ].map(function(b) {
				var x = r.bits.filter(function(e) { return e.dir == dir && e.bit == b; })[0];
				if (!x)
					return E('td', { 'class': 'td center ls-muted', 'title': 'a CPLD não dirige esta linha' }, '-');
				return E('td', { 'class': 'td center', 'title': x.checks + ' verificações, ' + x.failed + ' falhas' },
					E('span', { 'class': 'ls-level ' + (x.failed ? 'ls-level-bad' : 'ls-level-hi') }, x.failed ? x.failed + ' falha(s)' : 'ok'));
			})));
		};

		return E('div', {}, [
			E('p', {}, [ E('strong', {}, r.failed ? r.failed + ' falha(s)' : 'Tudo certo'), ' em ', r.checks, ' verificações, às ', new Date().toLocaleTimeString(), '.' ]),
			E('div', { 'class': 'ls-scroll' }, E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [ E('th', { 'class': 'th' }, 'Sentido') ].concat([ 0, 1, 2, 3, 4, 5, 6, 7 ].map(function(b) {
					return E('th', { 'class': 'th center' }, 'IO1_' + b + ' ↔ GPX' + (8 + b));
				}))),
				row('cpld>pca', 'CPLD dirige, PCA9555 lê'),
				row('pca>cpld', 'PCA9555 dirige, CPLD lê')
			]))
		]);
	},

	renderHeader: function(lines, pins, conn) {
		var byLine = {};
		lines.forEach(function(l) { byLine[l.line] = l; });
		var row = function(odd) {
			var cells = [];
			for (var p = odd ? 1 : 2; p <= 20; p += 2) {
				var w = pins[p], l = (typeof w == 'number') ? byLine[w] : null;
				cells.push(E('div', {
					'class': 'ls-pin' + (l ? ' ls-pin-io' + (l.value ? ' ls-pin-hi' : '') + (l.dir == 'out' ? ' ls-pin-out' : '') : '') +
						(w == 'GND' ? ' ls-pin-gnd' : '') + (/V$/.test(w) ? ' ls-pin-pwr' : ''),
					'title': conn + '.' + p + (l ? ': ' + name(w) + ', ' + (l.dir == 'out' ? 'saída' : 'entrada') + ', ' + l.value : '')
				}, [ E('small', {}, p), E('span', {}, typeof w == 'number' ? name(w) : w) ]));
			}
			return E('div', { 'class': 'ls-pinrow' }, cells);
		};
		return E('div', { 'class': 'ls-header' }, [ row(true), row(false) ]);
	},

	renderTable: function(lines) {
		var self = this;
		if (!lines.length)
			return E('p', { 'class': 'ls-muted' }, 'O PCA9555 não foi encontrado (gpiochip 0-0023).');

		var cpld = lines.filter(function(l) { return l.line >= CPLD_FIRST && l.line < CPU_FIRST; });
		var cpu = lines.filter(function(l) { return l.line >= CPU_FIRST; });
		return E('div', {}, [
			E('h4', {}, [ 'PCA9555 (U2) ', E('a', { 'href': common.url('pca9555') }, '→') ]),
			this.renderLines(lines.filter(function(l) { return l.line < CPLD_FIRST; })),
			E('h4', {}, [ 'CPLD (U33) ', E('a', { 'href': common.url('cpld') }, '→') ]),
			cpld.length ? this.renderLines(cpld) :
				E('p', { 'class': 'ls-muted' }, 'As E/S da CPLD precisam da imagem 3.0 e do kmod-ls1021aiot-cpld novo.'),
			E('h4', {}, [ 'LS1021A, GPIO3 (U22) ', E('a', { 'href': common.url('soc') }, '→') ]),
			cpu.length ? this.renderLines(cpu) :
				E('p', { 'class': 'ls-muted' }, 'O controlador GPIO3 (2320000.gpio) não foi encontrado.')
		]);
	},

	renderLines: function(lines) {
		var self = this;

		var btn = function(label, active, fn) {
			return E('button', {
				'class': 'btn cbi-button' + (active ? ' cbi-button-positive' : ''),
				'click': ui.createHandlerFn(self, fn)
			}, label);
		};

		return E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, 'Linha'),
				E('th', { 'class': 'th' }, 'Pino'),
				E('th', { 'class': 'th' }, 'Modo'),
				E('th', { 'class': 'th' }, 'Valor')
			])
		].concat(lines.map(function(l) {
			var out = l.dir == 'out';
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, E('strong', {}, name(l.line))),
				E('td', { 'class': 'td', 'style': 'white-space:nowrap' }, WHERE[l.line]),
				E('td', { 'class': 'td' }, E('div', { 'class': 'ls-toolbar', 'style': 'margin:0' }, [
					btn('Entrada', !out, function() { return out ? self.handleSet(l.line, 'in', 0) : null; }),
					/* a line made an output starts at the level it reads now */
					IN_ONLY[l.line] ? '' : btn('Saída', out, function() { return out ? null : self.handleSet(l.line, 'out', l.value); })
				])),
				E('td', { 'class': 'td' }, out
					? E('div', { 'class': 'ls-toolbar', 'style': 'margin:0' }, [
						btn('0', !l.value, function() { return self.handleSet(l.line, 'out', 0); }),
						btn('1', !!l.value, function() { return self.handleSet(l.line, 'out', 1); })
					])
					: E('span', { 'class': 'ls-level' + (l.value ? ' ls-level-hi' : '') }, l.value ? '1 (alto)' : '0 (baixo)'))
			]);
		})));
	},

	render: function(lines) {
		this.lines = lines;
		this.stamp = E('span', { 'class': 'ls-muted' }, '');
		this.header = E('div', {}, this.renderHeader(lines, J501, 'J501'));
		this.header2 = E('div', {}, this.renderHeader(lines, J502, 'J502'));
		this.table = E('div', { 'class': 'ls-scroll' }, this.renderTable(lines));
		this.loop = E('div', {});

		poll.add(L.bind(this.refresh, this), 1);

		return E('div', {}, [
			common.css,
			E('style', {}, [
				'.ls-header{display:inline-flex;flex-direction:column;gap:3px;padding:6px;background:#1b1b1b;border-radius:6px;max-width:100%;overflow-x:auto}',
				'.ls-pinrow{display:flex;gap:3px}',
				'.ls-pin{width:62px;min-width:62px;height:40px;border:1px solid #555;border-radius:4px;background:#2b2b2b;color:#bbb;display:flex;flex-direction:column;align-items:center;justify-content:center;font-size:10.5px;line-height:1.2}',
				'.ls-pin small{font-size:9px;color:#888}',
				'.ls-pin-gnd{color:#888}',
				'.ls-pin-pwr{color:#ef9a9a}',
				'.ls-pin-io{border:2px dashed #4fc3f7;color:#e0e0e0}',
				'.ls-pin-out{border-style:solid;border-color:#ffc107}',
				'.ls-pin-hi{background:#2e7d32;color:#fff}',
				'.ls-pin-hi small{color:#c8e6c9}',
				'.ls-level{display:inline-block;padding:2px 10px;border-radius:10px;background:#424242;color:#ddd}',
				'.ls-level-hi{background:#2e7d32;color:#fff}',
				'.ls-level-bad{background:#c62828;color:#fff}'
			].join('')),
			E('h2', {}, 'E/S digitais'),
			E('p', { 'class': 'cbi-section-descr' }, [
				'As 16 linhas do expansor ', E('a', { 'href': common.url('pca9555') }, 'PCA9555 (U2)'), ', no I2C, e as 28 da ',
				E('a', { 'href': common.url('cpld') }, 'CPLD (U33)'), ', no IFC. ',
				'Em entrada, a linha mostra o nível que lê; em saída, você escolhe o nível que ela fornece. ',
				'IO0_0 a IO0_7 do PCA9555 e CPLD0 a CPLD5 estão no conector J501 (PORT 1, no painel de trás). ',
				'IO1_0 a IO1_7 do PCA9555 são ligadas direto a GPX8 a GPX15 da CPLD: deixe um dos lados em entrada. ',
				'D2 a D7 são os pinos digitais do header Arduino, onde está a FRDM-KW41Z; TP9 a TP16 são pontos de teste. ',
				'GPIO3_15 a GPIO3_27, do próprio LS1021A, estão no J502 (PORT 2).'
			]),
			E('div', { 'class': 'alert-message warning' }, [
				'Os pinos trabalham a 3,3 V: não ligue neles mais que isso. Em saída, use cargas leves (uma entrada lógica, um LED com resistor) ',
				'e não ligue a saída a outra saída nem direto ao GND ou ao 3,3 V. ',
				'As linhas voltam a ser entradas quando a placa é desligada (as da CPLD também num reset a frio). ',
				'CPLD0 a CPLD4 só viram saída com o mux QE/LCD da CPLD em LCD, o padrão.'
			]),
			E('h3', {}, [ 'J501 (PORT 1) ', E('span', { 'class': 'ls-muted', 'style': 'font-size:.8em;font-weight:normal' }, 'tracejado: entrada · contorno amarelo: saída · verde: nível 1') ]),
			this.header,
			E('h3', {}, [ 'J502 (PORT 2) ', E('span', { 'class': 'ls-muted', 'style': 'font-size:.8em;font-weight:normal' }, 'GPIOs do LS1021A, a 2,5 V') ]),
			this.header2,
			E('div', { 'class': 'alert-message warning', 'style': 'margin-top:.6em' }, [
				'Os pinos do J502 são do próprio processador e trabalham a 2,5 V (LVDD): não ligue neles 3,3 V nem 5 V. ',
				'Eles só funcionam como GPIO com o campo EC2 do RCW em 001 (GPIO3[15:27]); com o RCW de fábrica (EC2 = 010) ',
				'eles são CAN3/CAN4 e leem sempre 0.'
			]),
			E('h3', {}, 'Teste de loopback: CPU ↔ CPLD'),
			E('p', { 'class': 'cbi-section-descr' }, [
				'IO1_0 a IO1_7 do PCA9555 (que a CPU acessa pelo I2C) são ligadas direto a GPX8 a GPX15 da CPLD (pelo IFC). ',
				'O teste põe padrões de um lado e lê do outro, nos dois sentidos, e depois devolve as 16 linhas ao modo em que estavam. ',
				'Uma falha aponta uma ligação aberta, em curto ou presa, ou um problema no caminho I2C ou IFC.'
			]),
			E('div', { 'class': 'ls-toolbar' }, E('button', {
				'class': 'btn cbi-button cbi-button-action',
				'click': ui.createHandlerFn(this, 'handleLoopback')
			}, 'Testar as ligações')),
			this.loop,
			E('h3', {}, [ 'Linhas ', this.stamp ]),
			this.table
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
