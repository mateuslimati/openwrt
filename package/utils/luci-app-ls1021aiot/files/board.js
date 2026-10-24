'use strict';
'require view';
'require poll';
'require rpc';
'require ui';

/*
 * LS1021A-IoT: temperature, voltage, current and power graphs from the
 * ls1021aiot-sensord history, and the switch ports.
 */

var callStatus = rpc.declare({
	object: 'ls1021aiot',
	method: 'status'
});

var callResources = rpc.declare({
	object: 'ls1021aiot',
	method: 'resources'
});

var callControl = rpc.declare({
	object: 'ls1021aiot',
	method: 'control',
	params: [ 'action', 'value' ]
});

var callHistory = rpc.declare({
	object: 'ls1021aiot',
	method: 'history',
	params: [ 'points', 'seconds' ],
	expect: { rows: [] }
});

var callSetPort = rpc.declare({
	object: 'ls1021aiot',
	method: 'set_port',
	params: [ 'port', 'on' ]
});

/* columns of a history row */
var T = 0, DIE = 1, ADT = 2, LM75 = 3, IN12_MV = 4, IN12_MA = 5, IN12_MW = 6,
    RAIL_MV = 7, RAIL_MA = 8, RAIL_MW = 9, FAN = 10;

var charts = [
	{
		title: 'Temperaturas', unit: '°C', digits: 1,
		lines: [
			{ name: 'Die LS1021A', col: DIE, scale: 1000, color: '#d9534f' },
			{ name: 'Placa (ADT7461)', col: ADT, scale: 1000, color: '#f0ad4e' },
			{ name: 'Placa (LM75A)', col: LM75, scale: 1000, color: '#5bc0de' }
		]
	},
	{
		title: 'Tensão da entrada 12 V', unit: 'V', digits: 2,
		lines: [ { name: 'Entrada', col: IN12_MV, scale: 1000, color: '#337ab7' } ]
	},
	{
		title: 'Tensão do trilho 1,0 V', unit: 'V', digits: 3,
		lines: [ { name: 'Trilho', col: RAIL_MV, scale: 1000, color: '#5cb85c' } ]
	},
	{
		title: 'Corrente', unit: 'A', digits: 3,
		lines: [
			{ name: 'Entrada 12 V', col: IN12_MA, scale: 1000, color: '#337ab7' },
			{ name: 'Trilho 1,0 V', col: RAIL_MA, scale: 1000, color: '#5cb85c' }
		]
	},
	{
		title: 'Potência', unit: 'W', digits: 2,
		lines: [
			{ name: 'Entrada 12 V', col: IN12_MW, scale: 1000, color: '#337ab7' },
			{ name: 'Trilho 1,0 V', col: RAIL_MW, scale: 1000, color: '#5cb85c' }
		]
	}
];

var ranges = [
	{ label: '1 hora', seconds: 3600 },
	{ label: '6 horas', seconds: 21600 },
	{ label: '24 horas', seconds: 86400 }
];

var SVGNS = 'http://www.w3.org/2000/svg';

function svgEl(name, attrs, text) {
	var el = document.createElementNS(SVGNS, name);

	for (var k in attrs)
		el.setAttribute(k, attrs[k]);
	if (text != null)
		el.appendChild(document.createTextNode(text));
	return el;
}

function clock(t) {
	var d = new Date(t * 1000);

	return ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2);
}

/* a line chart of the rows, scaled to the data, with the fan periods shaded */
function drawChart(chart, rows, seconds) {
	var W = 800, H = 220, L = 60, R = 24, TOP = 10, B = 24;
	var svg = svgEl('svg', { viewBox: '0 0 ' + W + ' ' + H, width: '100%',
				 preserveAspectRatio: 'none', style: 'height:220px' });
	var now = rows.length ? rows[rows.length - 1][T] : Date.now() / 1000;
	var t0 = now - seconds, min = Infinity, max = -Infinity;

	rows.forEach(function(r) {
		chart.lines.forEach(function(l) {
			var v = r[l.col] / l.scale;
			min = Math.min(min, v);
			max = Math.max(max, v);
		});
	});

	if (!rows.length) {
		svg.appendChild(svgEl('text', { x: W / 2, y: H / 2, 'text-anchor': 'middle',
						'fill': '#888' }, 'Sem dados ainda'));
		return svg;
	}

	/* some margin, and never a flat range */
	var pad = Math.max((max - min) * 0.1, Math.abs(max) * 0.01, Math.pow(10, -chart.digits));
	min -= pad;
	max += pad;

	var x = function(t) { return L + (t - t0) / seconds * (W - L - R); };
	var y = function(v) { return TOP + (max - v) / (max - min) * (H - TOP - B); };

	/* fan on */
	if (chart.lines[0].col == DIE) {
		var start = null;

		rows.concat([ [ now + 1 ] ]).forEach(function(r) {
			var on = r[FAN] == 1;

			if (on && start == null)
				start = r[T];
			else if (!on && start != null) {
				svg.appendChild(svgEl('rect', { x: x(start), y: TOP,
					width: Math.max(x(r[T]) - x(start), 1), height: H - TOP - B,
					fill: '#5bc0de', 'fill-opacity': 0.15 }));
				start = null;
			}
		});
	}

	for (var i = 0; i <= 4; i++) {
		var v = min + (max - min) * i / 4, yy = y(v);

		svg.appendChild(svgEl('line', { x1: L, x2: W - R, y1: yy, y2: yy,
						stroke: '#ccc', 'stroke-width': 0.5 }));
		svg.appendChild(svgEl('text', { x: L - 6, y: yy + 4, 'text-anchor': 'end',
						'font-size': 11, fill: '#888' },
				      v.toFixed(chart.digits)));
	}

	for (var i = 0; i <= 6; i++) {
		var t = t0 + seconds * i / 6, xx = x(t);

		svg.appendChild(svgEl('line', { x1: xx, x2: xx, y1: TOP, y2: H - B,
						stroke: '#eee', 'stroke-width': 0.5 }));
		svg.appendChild(svgEl('text', { x: xx, y: H - 6, 'text-anchor': 'middle',
						'font-size': 11, fill: '#888' }, clock(t)));
	}

	chart.lines.forEach(function(l) {
		var pts = rows.map(function(r) {
			return x(r[T]).toFixed(1) + ',' + y(r[l.col] / l.scale).toFixed(1);
		});

		svg.appendChild(svgEl('polyline', { points: pts.join(' '), fill: 'none',
						    stroke: l.color, 'stroke-width': 1.5,
						    'vector-effect': 'non-scaling-stroke' }));
	});

	return svg;
}

function legend(chart, last) {
	return E('div', { 'style': 'display:flex;flex-wrap:wrap;gap:1em;margin:.3em 0 1em' },
		chart.lines.map(function(l) {
			var v = last ? (last[l.col] / l.scale).toFixed(chart.digits) + ' ' + chart.unit : '-';

			return E('span', {}, [
				E('span', { 'style': 'display:inline-block;width:1em;height:3px;' +
						     'vertical-align:middle;margin-right:.4em;background:' + l.color }),
				l.name + ': ', E('strong', {}, v)
			]);
		}));
}

return view.extend({
	seconds: 3600,

	load: function() {
		return Promise.all([
			L.resolveDefault(callStatus(), null),
			L.resolveDefault(callHistory(720, this.seconds), []),
			L.resolveDefault(callResources(), null)
		]);
	},

	handleControl: function(action, value, ev) {
		return callControl(action, value).then(L.bind(this.refresh, this));
	},

	handlePort: function(port, on, ev) {
		var msg = on ? null :
			'Desligar a porta ' + port.jack + '? Se o computador ou o roteador ' +
			'por onde você acessa a placa estiver nela, a LuCI perde a conexão ' +
			'até a porta ser religada pelo Bluetooth ou a placa reiniciar.';

		if (msg && !confirm(msg))
			return;

		return callSetPort(port.port, on).then(L.bind(this.refresh, this));
	},

	renderStatus: function(st) {
		if (!st)
			return E('p', { 'class': 'alert-message warning' },
				 'Sem resposta do plugin ls1021aiot do rpcd.');

		var c = function(mc) { return (mc / 1000).toFixed(1) + ' °C'; };
		var rows = [
			[ 'Die LS1021A', c(st.temp.die) ],
			[ 'Placa (ADT7461)', c(st.temp.adt7461) ],
			[ 'Placa (LM75A)', c(st.temp.lm75) ],
			[ 'Entrada 12 V', '%.2f V, %.3f A, %.2f W'.format(st.in12.mv / 1000,
				st.in12.ma / 1000, st.in12.mw / 1000) ],
			[ 'Trilho 1,0 V', '%.3f V, %.3f A, %.2f W'.format(st.rail1.mv / 1000,
				st.rail1.ma / 1000, st.rail1.mw / 1000) ],
			[ 'Ventilador', (st.fan.on ? 'ligado' : 'desligado') +
				' (liga a %d °C, desliga a %d °C)'.format(st.fan.on_mc / 1000,
									 st.fan.off_mc / 1000) ]
		];

		return E('table', { 'class': 'table' }, rows.map(function(r) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, r[0]),
				E('td', { 'class': 'td left' }, E('strong', {}, r[1]))
			]);
		}));
	},

	renderPorts: function(st) {
		var self = this;

		if (!st)
			return E('div');

		return E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, 'Porta'),
				E('th', { 'class': 'th' }, 'Estado'),
				E('th', { 'class': 'th' }, 'Link'),
				E('th', { 'class': 'th' }, '')
			])
		].concat(st.ports.map(function(p) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, E('strong', {}, p.jack)),
				E('td', { 'class': 'td' }, p.disabled ? 'desligada' : 'ligada'),
				E('td', { 'class': 'td' }, p.disabled ? '-' :
					(p.link ? p.speed + ' Mb/s' : 'sem link')),
				E('td', { 'class': 'td right' }, E('button', {
					'class': 'btn cbi-button ' + (p.disabled ? 'cbi-button-apply' : 'cbi-button-reset'),
					'click': ui.createHandlerFn(self, 'handlePort', p, !!p.disabled)
				}, p.disabled ? 'Ligar' : 'Desligar'))
			]);
		})));
	},

	renderTable: function(rows) {
		return E('table', { 'class': 'table' }, rows.map(function(r) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, r[0]),
				E('td', { 'class': 'td left' }, E('strong', {}, r[1]))
			]);
		}));
	},

	renderBoard: function(res) {
		var b = res.board || {}, sys = res.system || {}, rows = [];
		var uptime = sys.uptime || 0;

		if (b.serial)
			rows.push([ 'Número de série', b.serial ],
				  [ 'Revisão da placa', b.revision ],
				  [ 'Fabricação', b.date ],
				  [ 'MACs (EEPROM)', (b.macs || []).join(', ') ]);

		/* SW2: a switch ON reads 0 */
		if (b.dip_switch) {
			var v = parseInt(b.dip_switch, 16), sw = [];
			for (var i = 0; i < 5; i++)
				sw.push((i + 1) + ':' + (v & (1 << i) ? 'off' : 'ON'));
			rows.push([ 'Chaves SW2', sw.join('  ') + ' (' + b.dip_switch + ')' ]);
		}

		rows.push([ 'SoC', '%s, revisão %s (%s)'.format(sys.family, sys.revision, sys.soc_id) ],
			  [ 'CPU', '%d núcleos Cortex-A7 a %d MHz'.format(sys.cpus, sys.cpu_mhz) ],
			  [ 'Memória', '%d MiB livres de %d MiB'.format(sys.mem_avail_kb / 1024,
									 sys.mem_total_kb / 1024) ],
			  [ 'Carga', sys.load ],
			  [ 'Ligada há', '%dd %02dh%02dm'.format(uptime / 86400, uptime / 3600 % 24,
								  uptime / 60 % 60) ],
			  [ 'Kernel', sys.kernel ]);

		return this.renderTable(rows);
	},

	renderAccel: function(res) {
		var a = res.accel || {};

		if (a.x == null || a.x === '')
			return E('p', {}, 'Acelerômetro sem resposta.');

		var x = +a.x, y = +a.y, z = +a.z, g = 9.80665;
		var deg = function(r) { return (r * 180 / Math.PI).toFixed(1) + '°'; };

		return this.renderTable([
			[ 'X, Y, Z', '%.2f, %.2f, %.2f m/s² (%.2f, %.2f, %.2f g)'.format(x, y, z,
				x / g, y / g, z / g) ],
			[ 'Inclinação', 'frente/trás %s, lateral %s'.format(
				deg(Math.atan2(x, Math.sqrt(y * y + z * z))),
				deg(Math.atan2(y, Math.sqrt(x * x + z * z)))) ],
			[ 'Módulo', '%.2f g'.format(Math.sqrt(x * x + y * y + z * z) / g) ]
		]);
	},

	renderAdc: function(res) {
		var adc = res.adc || [];

		if (!adc.length)
			return E('p', {}, 'ADC sem resposta.');

		return E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, adc.map(function(v, i) {
				return E('th', { 'class': 'th center' }, 'AIN' + i);
			})),
			E('tr', { 'class': 'tr' }, adc.map(function(v) {
				return E('td', { 'class': 'td center' }, v >= 2048 ? '≥ 2048' : v);
			}))
		]);
	},

	renderGpio: function(res) {
		var gpio = res.gpio || [];

		if (!gpio.length)
			return E('p', {}, 'PCA9555 sem resposta.');

		return E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, gpio.map(function(v, i) {
				return E('th', { 'class': 'th center' }, (i < 8 ? 'P0.' : 'P1.') + (i % 8));
			})),
			E('tr', { 'class': 'tr' }, gpio.map(function(v) {
				return E('td', { 'class': 'td center' }, v ? '1' : '0');
			}))
		]);
	},

	renderControls: function(res) {
		var self = this, led = (res.led0 || {}).mode, bt = res.bluetooth || {};
		var btn = function(label, action, value, active) {
			return E('button', {
				'class': 'btn cbi-button' + (active ? ' cbi-button-positive' : ''),
				'click': ui.createHandlerFn(self, 'handleControl', action, value)
			}, label);
		};
		var row = function(label, items) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, label),
				E('td', { 'class': 'td left' },
				  E('div', { 'style': 'display:flex;flex-wrap:wrap;gap:.5em' }, items))
			]);
		};

		return E('table', { 'class': 'table' }, [
			row('LED0 (CPLD)', [ btn('Apagado', 'led0', 'off', led == 'off'),
					     btn('Aceso', 'led0', 'on', led == 'on'),
					     btn('Piscando', 'led0', 'blink', led == 'blink') ]),
			row('Sons (saída J2)', [ btn('Boot', 'sound', 'boot-sound'),
						 btn('Conexão BLE', 'sound', 'bt-connect'),
						 btn('Desconexão BLE', 'sound', 'bt-disconnect') ]),
			row('Shield Arduino', [ btn('Resetar (KW41Z)', 'shield_reset', '') ]),
			row('Bluetooth', [ E('strong', {}, !bt.up ? 'hci0 parado' :
				(bt.central ? 'conectado a ' + bt.central : 'anunciando como "spartan"')) ])
		]);
	},

	renderCharts: function(rows) {
		var last = rows.length ? rows[rows.length - 1] : null, seconds = this.seconds;

		return E('div', {}, charts.map(function(chart) {
			return E('div', {}, [
				E('h4', {}, chart.title + (chart.lines[0].col == DIE ?
					' (faixa azul: ventilador ligado)' : '')),
				drawChart(chart, rows, seconds),
				legend(chart, last)
			]);
		}));
	},

	update: function(st, rows, res) {
		var view = this.node;

		if (res) {
			view.querySelector('#board').replaceChildren(this.renderBoard(res));
			view.querySelector('#controls').replaceChildren(this.renderControls(res));
			view.querySelector('#accel').replaceChildren(this.renderAccel(res));
			view.querySelector('#adc').replaceChildren(this.renderAdc(res));
			view.querySelector('#gpio').replaceChildren(this.renderGpio(res));
		}
		view.querySelector('#status').replaceChildren(this.renderStatus(st));
		view.querySelector('#ports').replaceChildren(this.renderPorts(st));
		view.querySelector('#charts').replaceChildren(this.renderCharts(rows));
	},

	refresh: function() {
		return Promise.all([
			L.resolveDefault(callStatus(), null),
			L.resolveDefault(callHistory(720, this.seconds), []),
			L.resolveDefault(callResources(), null)
		]).then(L.bind(function(data) {
			this.update(data[0], data[1], data[2]);
		}, this));
	},

	render: function(data) {
		var self = this;
		var buttons = ranges.map(function(r) {
			return E('button', {
				'class': 'btn cbi-button' + (r.seconds == self.seconds ? ' cbi-button-positive' : ''),
				'click': function(ev) {
					self.seconds = r.seconds;
					buttons.forEach(function(b) { b.classList.remove('cbi-button-positive'); });
					ev.target.classList.add('cbi-button-positive');
					return self.refresh();
				}
			}, r.label);
		});

		var node = E('div', { 'id': 'ls1021aiot' }, [
			E('h2', {}, 'LS1021A-IoT'),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, 'Placa e sistema'),
				E('div', { 'id': 'board' })
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, 'Controles'),
				E('div', { 'id': 'controls' })
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, 'Sensores'),
				E('div', { 'id': 'status' })
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, 'Acelerômetro (MMA8451Q)'),
				E('div', { 'id': 'accel' })
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, 'Entradas analógicas do J500 (MAX1239, mV, referência 2,048 V)'),
				E('div', { 'id': 'adc' })
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, 'GPIO do PCA9555 (só leitura, todas entradas)'),
				E('div', { 'id': 'gpio' })
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, 'Portas do switch'),
				E('p', {}, 'Desligar uma porta corta a alimentação do PHY dela. ' +
					   'Todas voltam ligadas quando a placa reinicia.'),
				E('div', { 'id': 'ports' })
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, 'Histórico'),
				E('div', { 'style': 'display:flex;gap:.5em;margin-bottom:1em' }, buttons),
				E('div', { 'id': 'charts' })
			])
		]);

		this.node = node;

		/* replaceChildren is missing on older browsers */
		if (!Element.prototype.replaceChildren)
			Element.prototype.replaceChildren = function() {
				while (this.firstChild)
					this.removeChild(this.firstChild);
				for (var i = 0; i < arguments.length; i++)
					this.appendChild(arguments[i]);
			};

		this.update(data[0], data[1], data[2]);
		poll.add(L.bind(this.refresh, this), 10);

		return node;
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
