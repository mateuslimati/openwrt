'use strict';
'require view';
'require poll';
'require rpc';
'require ui';
'require ls1021aiot.common as common';

/*
 * The 16 lines of the PCA9555 (U2): the mode of each (input or output),
 * the level it reads as an input and the level to drive as an output.
 * IO0_0..IO0_7 are on J501 (PORT 1 on the rear panel), IO1_0..IO1_7 go
 * to the CPLD, which leaves them unused.
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

/* line -> where it goes */
var WHERE = [
	'J501.3', 'J501.5', 'J501.9', 'J501.11', 'J501.13', 'J501.17', 'J501.19', 'J501.18',
	'CPLD D6', 'CPLD D5', 'CPLD B4', 'CPLD B5', 'CPLD A5', 'CPLD A4', 'CPLD C6', 'CPLD C7'
];

/* J501, pin -> what is on it: a line of the PCA9555 (number) or a name */
var J501 = {
	1: '3,3 V', 2: 'CPLD0', 3: 0, 4: '5 V', 5: 1, 6: 'CPLD1', 7: 'GND', 8: 'CPLD2', 9: 2, 10: 'CPLD3',
	11: 3, 12: 'GND', 13: 4, 14: 'CPLD4', 15: 'GND', 16: 'CPLD5', 17: 5, 18: 7, 19: 6, 20: 'GND'
};

function name(i) {
	return i < 8 ? 'IO0_' + i : 'IO1_' + (i - 8);
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
			this.header.replaceChildren(this.renderHeader(lines));
			this.stamp.textContent = 'atualizado às ' + new Date().toLocaleTimeString();
		}, this));
	},

	renderHeader: function(lines) {
		var row = function(odd) {
			var cells = [];
			for (var p = odd ? 1 : 2; p <= 20; p += 2) {
				var w = J501[p], l = (typeof w == 'number') ? lines[w] : null;
				cells.push(E('div', {
					'class': 'ls-pin' + (l ? ' ls-pin-io' + (l.value ? ' ls-pin-hi' : '') + (l.dir == 'out' ? ' ls-pin-out' : '') : '') +
						(w == 'GND' ? ' ls-pin-gnd' : '') + (/V$/.test(w) ? ' ls-pin-pwr' : ''),
					'title': 'J501.' + p + (l ? ': ' + name(w) + ', ' + (l.dir == 'out' ? 'saída' : 'entrada') + ', ' + l.value : '')
				}, [ E('small', {}, p), E('span', {}, l ? name(w) : w) ]));
			}
			return E('div', { 'class': 'ls-pinrow' }, cells);
		};
		return E('div', { 'class': 'ls-header' }, [ row(true), row(false) ]);
	},

	renderTable: function(lines) {
		var self = this;
		if (!lines.length)
			return E('p', { 'class': 'ls-muted' }, 'O PCA9555 não foi encontrado (gpiochip 0-0023).');

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
					btn('Saída', out, function() { return out ? null : self.handleSet(l.line, 'out', l.value); })
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
		this.header = E('div', {}, this.renderHeader(lines));
		this.table = E('div', { 'class': 'ls-scroll' }, this.renderTable(lines));

		poll.add(L.bind(this.refresh, this), 1);

		return E('div', {}, [
			common.css,
			E('style', {}, [
				'.ls-header{display:inline-flex;flex-direction:column;gap:3px;padding:6px;background:#1b1b1b;border-radius:6px;max-width:100%;overflow-x:auto}',
				'.ls-pinrow{display:flex;gap:3px}',
				'.ls-pin{width:52px;min-width:52px;height:40px;border:1px solid #555;border-radius:4px;background:#2b2b2b;color:#bbb;display:flex;flex-direction:column;align-items:center;justify-content:center;font-size:11px;line-height:1.2}',
				'.ls-pin small{font-size:9px;color:#888}',
				'.ls-pin-gnd{color:#888}',
				'.ls-pin-pwr{color:#ef9a9a}',
				'.ls-pin-io{border:2px dashed #4fc3f7;color:#e0e0e0}',
				'.ls-pin-out{border-style:solid;border-color:#ffc107}',
				'.ls-pin-hi{background:#2e7d32;color:#fff}',
				'.ls-pin-hi small{color:#c8e6c9}',
				'.ls-level{display:inline-block;padding:2px 10px;border-radius:10px;background:#424242;color:#ddd}',
				'.ls-level-hi{background:#2e7d32;color:#fff}'
			].join('')),
			E('h2', {}, 'E/S digitais'),
			E('p', { 'class': 'cbi-section-descr' }, [
				'As 16 linhas do expansor ', E('a', { 'href': common.url('pca9555') }, 'PCA9555 (U2)'), ', no I2C. ',
				'Em entrada, a linha mostra o nível que lê; em saída, você escolhe o nível que ela fornece. ',
				'IO0_0 a IO0_7 estão no conector J501 (PORT 1, no painel de trás); IO1_0 a IO1_7 vão ao CPLD, que não as usa.'
			]),
			E('div', { 'class': 'alert-message warning' }, [
				'Os pinos trabalham a 3,3 V: não ligue neles mais que isso. Em saída, use cargas leves (uma entrada lógica, um LED com resistor) ',
				'e não ligue a saída a outra saída nem direto ao GND ou ao 3,3 V. ',
				'As linhas voltam a ser entradas quando a placa é desligada.'
			]),
			E('h3', {}, [ 'J501 (PORT 1) ', E('span', { 'class': 'ls-muted', 'style': 'font-size:.8em;font-weight:normal' }, 'tracejado: entrada · contorno amarelo: saída · verde: nível 1') ]),
			this.header,
			E('h3', {}, [ 'Linhas ', this.stamp ]),
			this.table
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
