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
		this.stamp.textContent = 'atualizado às ' + new Date().toLocaleTimeString();
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
				'.ls-bar-range{position:absolute;bottom:0;height:3px;background:#ffc107}'
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
