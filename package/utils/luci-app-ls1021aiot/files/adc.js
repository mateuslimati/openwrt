'use strict';
'require view';
'require poll';
'require rpc';
'require ls1021aiot.common as common';

/*
 * The 12 inputs of the MAX1239 (U6) in volts, read once a second, with
 * the lowest and highest seen since the page was opened. CH0..CH3 are
 * the Arduino A0..A3 (J15), CH4..CH11 are on J500 (PORT 0).
 */

var callAdc = rpc.declare({
	object: 'ls1021aiot',
	method: 'adc'
});

/* channel -> where it is */
var WHERE = [
	'J15.1 (Arduino A0)', 'J15.2 (Arduino A1)', 'J15.3 (Arduino A2)', 'J15.4 (Arduino A3)',
	'J500.3', 'J500.5', 'J500.7', 'J500.9', 'J500.2', 'J500.4', 'J500.6', 'J500.8'
];
var FULL = 4095;

/* J500 (PORT 0), pin -> an ADC channel (number) or a name */
var J500 = {
	1: '3,3 V', 2: 8, 3: 4, 4: 9, 5: 5, 6: 10, 7: 6, 8: 11, 9: 7, 10: '3,3 V',
	11: 'GND', 12: 'SPI1_CS0', 13: 'UART_CTS', 14: 'SPI1_SOUT', 15: 'UART_RTS', 16: 'SPI1_SIN',
	17: 'UART_RX', 18: 'SPI1_SCK', 19: 'UART_TX', 20: 'GND'
};
/* what the other pins are */
var J500_INFO = {
	12: 'SPI1_PCS0 do LS1021A (o mesmo SPI1 do switch, que usa o CS1)',
	13: 'LPUART1_CTS_B', 14: 'SPI1_SOUT (MOSI)', 15: 'LPUART1_RTS_B', 16: 'SPI1_SIN (MISO)',
	17: 'LPUART1_SIN', 18: 'SPI1_SCK', 19: 'LPUART1_SOUT'
};

function volts(raw, scale) {
	return (raw * scale / 1000).toFixed(3).replace('.', ',') + ' V';
}

return view.extend({
	load: function() {
		return L.resolveDefault(callAdc(), {});
	},

	update: function(res) {
		var raw = res.raw || [], scale = +res.scale || 0;
		this.res = res;
		raw.forEach(L.bind(function(r, i) {
			var m = this.seen[i] || (this.seen[i] = { min: r, max: r });
			m.min = Math.min(m.min, r);
			m.max = Math.max(m.max, r);
		}, this));
		this.table.replaceChildren(this.renderTable(raw, scale));
		this.header.replaceChildren(this.renderHeader(raw, scale));
		this.stamp.textContent = 'atualizado às ' + new Date().toLocaleTimeString();
	},

	renderHeader: function(raw, scale) {
		var row = function(odd) {
			var cells = [];
			for (var p = odd ? 1 : 2; p <= 20; p += 2) {
				var w = J500[p], ch = typeof w == 'number', r = ch ? raw[w] : null, have = ch && r != null;
				var pct = have ? Math.min(100, 100 * r / FULL) : 0;
				cells.push(E('div', {
					'class': 'ls-pin' + (ch ? ' ls-pin-adc' + (have && r >= FULL ? ' ls-pin-full' : '') : '') +
						(w == 'GND' ? ' ls-pin-gnd' : '') + (/V$/.test(w) ? ' ls-pin-pwr' : '') + (J500_INFO[p] ? ' ls-pin-sig' : ''),
					'style': have ? 'background:linear-gradient(to top,#1565c0 ' + pct.toFixed(0) + '%,#2b2b2b ' + pct.toFixed(0) + '%)' : '',
					'title': 'J500.' + p + ': ' + (ch ? 'ADC CH' + w + (have ? ', ' + volts(r, scale) + (r >= FULL ? ' (no limite)' : '') : '') : (J500_INFO[p] || w))
				}, [
					E('small', {}, p),
					E('span', {}, ch ? 'CH' + w : w),
					have ? E('b', {}, (r * scale / 1000).toFixed(2).replace('.', ',')) : ''
				]));
			}
			return E('div', { 'class': 'ls-pinrow' }, cells);
		};
		return E('div', { 'class': 'ls-header' }, [ row(true), row(false) ]);
	},

	renderTable: function(raw, scale) {
		var self = this;
		if (!raw.length)
			return E('p', { 'class': 'ls-muted' }, 'O MAX1239 não foi encontrado (iio max1239).');

		return E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, 'Canal'),
				E('th', { 'class': 'th' }, 'Pino'),
				E('th', { 'class': 'th' }, 'Tensão'),
				E('th', { 'class': 'th', 'style': 'width:35%' }, ''),
				E('th', { 'class': 'th' }, 'Mín. / máx.'),
				E('th', { 'class': 'th' }, 'Bruto')
			])
		].concat(raw.map(function(r, i) {
			var m = self.seen[i] || { min: r, max: r };
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, E('strong', {}, 'CH' + i)),
				E('td', { 'class': 'td', 'style': 'white-space:nowrap' }, WHERE[i]),
				E('td', { 'class': 'td', 'style': 'white-space:nowrap' }, [
					E('strong', {}, volts(r, scale)),
					r >= FULL ? E('div', { 'class': 'ls-muted' }, 'no limite: 2,048 V ou mais') : ''
				]),
				E('td', { 'class': 'td' }, E('div', { 'class': 'ls-bar' }, [
					E('div', { 'class': 'ls-bar-fill' + (r >= FULL ? ' ls-bar-full' : ''), 'style': 'width:' + (100 * r / FULL).toFixed(1) + '%' }),
					E('div', { 'class': 'ls-bar-range', 'style': 'left:' + (100 * m.min / FULL).toFixed(1) + '%;width:' + (100 * (m.max - m.min) / FULL).toFixed(1) + '%' })
				])),
				E('td', { 'class': 'td', 'style': 'white-space:nowrap' }, volts(m.min, scale) + ' / ' + volts(m.max, scale)),
				E('td', { 'class': 'td' }, E('code', {}, r))
			]);
		})));
	},

	render: function(res) {
		var self = this;
		this.seen = [];
		this.stamp = E('span', { 'class': 'ls-muted' }, '');
		this.table = E('div', { 'class': 'ls-scroll' });
		this.header = E('div', {});
		this.update(res);

		poll.add(function() {
			return L.resolveDefault(callAdc(), {}).then(L.bind(self.update, self));
		}, 1);

		return E('div', {}, [
			common.css,
			E('style', {}, [
				'.ls-bar{position:relative;height:12px;min-width:80px;background:#333;border-radius:6px;overflow:hidden}',
				'.ls-bar-fill{position:absolute;left:0;top:0;bottom:0;background:#4fc3f7;transition:width .3s}',
				'.ls-bar-full{background:#ef5350}',
				'.ls-bar-range{position:absolute;bottom:0;height:3px;background:#ffc107}',
				'.ls-header{display:inline-flex;flex-direction:column;gap:3px;padding:6px;background:#1b1b1b;border-radius:6px;max-width:100%;overflow-x:auto}',
				'.ls-pinrow{display:flex;gap:3px}',
				'.ls-pin{width:62px;min-width:62px;height:48px;border:1px solid #555;border-radius:4px;background:#2b2b2b;color:#bbb;display:flex;flex-direction:column;align-items:center;justify-content:center;font-size:10px;line-height:1.2}',
				'.ls-pin small{font-size:9px;color:#888}',
				'.ls-pin b{font-size:12px;color:#fff}',
				'.ls-pin-gnd{color:#888}',
				'.ls-pin-pwr{color:#ef9a9a}',
				'.ls-pin-sig{color:#ce93d8}',
				'.ls-pin-adc{border:2px solid #4fc3f7;color:#e0e0e0}',
				'.ls-pin-full{border-color:#ef5350}'
			].join('')),
			E('h2', {}, 'Conversor A/D'),
			E('p', { 'class': 'cbi-section-descr' }, [
				'As 12 entradas do ', E('a', { 'href': common.url('max1239') }, 'MAX1239 (U6)'),
				', um conversor de 12 bits no I2C, lidas uma vez por segundo. ',
				'A escala vai de 0 a 2,048 V (a referência interna), ', (res.scale ? String(+res.scale).replace('.', ',') : '0,5'), ' mV por passo. ',
				'CH0 a CH3 são as entradas analógicas A0 a A3 do header Arduino (J15); CH4 a CH11 estão no J500 (PORT 0, no painel de trás).'
			]),
			E('div', { 'class': 'alert-message warning' }, [
				'Acima de 2,048 V a leitura para em 4095; não ligue mais que 3,3 V numa entrada. ',
				'Uma entrada solta (sem nada ligado) não lê 0 V: ela flutua, e o valor muda sozinho.'
			]),
			E('h3', {}, [ 'J500 (PORT 0) ', E('span', { 'class': 'ls-muted', 'style': 'font-size:.8em;font-weight:normal' }, 'em volts; a cor azul enche o pino até 2,048 V · roxo: SPI1 e LPUART1 do LS1021A') ]),
			this.header,
			E('h3', {}, [ 'Entradas ', this.stamp ]),
			this.table,
			E('div', { 'class': 'ls-toolbar', 'style': 'margin-top:.6em' }, [
				E('button', { 'class': 'btn cbi-button', 'click': function() {
					self.seen = [];
					self.update(self.res);
				} }, 'Zerar mín. / máx.'),
				E('span', { 'class': 'ls-muted' }, 'A faixa amarela embaixo de cada barra é o mínimo e o máximo vistos desde que a página foi aberta.')
			])
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
