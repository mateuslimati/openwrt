'use strict';
'require view';
'require poll';
'require rpc';
'require ui';
'require ls1021aiot.common as common';

/*
 * The system CPLD (U33), image 3.0: the reset of each chip, the board
 * resets, the watchdog, the controls, the status lines it reads and its
 * registers. Older images only show their DIP switches.
 */

var callCpld = rpc.declare({
	object: 'ls1021aiot',
	method: 'cpld'
});

var callCpldSet = rpc.declare({
	object: 'ls1021aiot',
	method: 'cpld_set',
	params: [ 'what', 'name', 'value' ]
});

/* reset name -> chip page, label, signal, what happens while it is in reset */
var RESETS = [
	[ 'switch', 'switch', 'Switch RTL8365MB (U31)', 'RST_ETH_SWITCH_B', 'ETH2 a ETH5 caem; o driver DSA não se recupera sozinho, pode ser preciso reiniciar a placa' ],
	[ 'sgmii1_phy', 'phy1', 'PHY AR8033 (U36)', 'RST_SGMII1_PHY_B', 'ETH0 cai e volta a negociar' ],
	[ 'sgmii2_phy', 'phy2', 'PHY AR8033 (U37)', 'RST_SGMII2_PHY_B', 'ETH1 cai e volta a negociar' ],
	[ 'usb_hub', 'usbhub', 'Hub USB5534B (U503)', 'RST_USBHUB_B', 'as tomadas USB 3.0 desconectam e voltam' ],
	[ 'arduino_shield', 'kw41z', 'Shield Arduino (FRDM-KW41Z)', 'RST_ARD_SHIELD_B', 'o Bluetooth cai; o serviço ls1021aiot_kw41z o reinicia' ],
	[ 'hdmi', 'hdmi', 'HDMI SiI9022A (U39)', 'RST_HDMI_B', 'o transmissor perde a configuração' ],
	[ 'pcie1', null, 'Mini PCIe P1', 'RST_PCIE1_B', 'o PERST# do slot' ],
	[ 'pcie2', null, 'Mini PCIe P2', 'RST_PCIE2_B', 'o PERST# do slot' ]
];

var CTRLS = [
	[ 'fan_force', 'Ventoinha sempre ligada', 'Sem isto, a ventoinha segue o pino THERM do ADT7461.' ],
	[ 'arduino_i2c_off', 'Desligar o I2C do Arduino', 'Desliga o repetidor PCA9515 (U10) entre o I2C1 e o header Arduino.' ],
	[ 'qspi_reset', 'Segurar a flash QSPI em reset', 'A placa dá boot pelo SD; a QSPI (U24) só guarda uma cópia do boot.' ]
];

var ACTIONS = [
	[ 'warm', 'Reset a quente', 'Só o PORESET do LS1021A, como o reboot normal. A CPLD e as fontes continuam ligadas.' ],
	[ 'cold', 'Reset a frio', 'Desliga o PMIC por 1 a 2 s e refaz a sequência de energia, como ao ligar a placa.' ],
	[ 'full', 'Reset total', 'Como apertar o SW1: o MAX811 reinicia a CPLD e o K22 também (a serial USB cai por um instante).' ]
];

var CAUSES = {
	power_on: 'Energização ou reset total (SW1)',
	cold: 'Reset a frio pedido por software',
	warm: 'Reset a quente pedido por software',
	watchdog_cold: 'Watchdog (reset a frio)',
	watchdog_warm: 'Watchdog (reset a quente)',
	reset_req: 'RESET_REQ_B do LS1021A (o reboot do Linux)'
};

/* status line -> label, level that means "ok" (null: just show it) */
var STATUS = [
	[ 'Energia', [
		[ 'ps_vcc33_pg', '3,3 V', 1 ], [ 'ps_vcc33_pmic_pg', '3,3 V do PMIC', 1 ], [ 'ps_vcc50_pg', '5 V', 1 ],
		[ 'pmic_por_b', 'POR do PMIC', 1 ], [ 'poe_stat', 'POE_STAT', null ], [ 'usbhub_vbus', 'VBUS do hub', 1 ],
		[ 'pwren', 'PWREN', null ], [ 'sda_swd_en', 'SWD do K22', null ] ] ],
	[ 'Temperatura', [
		[ 'therm_warn_b', 'ALERT do ADT7461', 1 ], [ 'therm_fault_b', 'THERM do ADT7461', 1 ],
		[ 'temp_monitor_os_b', 'OS do LM75', 1 ], [ 'fan_on', 'Ventoinha ligada', null ], [ 'cec_d', 'CEC do HDMI', null ] ] ],
	[ 'Interrupções (ativas em 0)', [
		[ 'sgmii1_phy_int_b', 'PHY U36', 1 ], [ 'sgmii2_phy_int_b', 'PHY U37', 1 ], [ 'eth_sw_int_b', 'Switch', 1 ],
		[ 'hdmi_irq', 'HDMI', null ], [ 'pmic_int_b', 'PMIC', 1 ], [ 'mma8451q_irq_b', 'MMA8451Q', 1 ],
		[ 'gpio_expand_int_b', 'PCA9555', 1 ], [ 'reset_req_b', 'RESET_REQ_B', 1 ] ] ],
	[ 'Processador', [
		[ 'evt0_b', 'EVT0', null ], [ 'evt1_b', 'EVT1', null ], [ 'evt2_b', 'EVT2', null ], [ 'evt3_b', 'EVT3', null ],
		[ 'evt4_b', 'EVT4', null ], [ 'jtag_hrst_b', 'JTAG HRST', 1 ], [ 'cpld_trst_b', 'TRST', null ], [ 'hreset_b', 'HRESET', 1 ] ] ],
	[ 'Sequência da CPLD', [
		[ 'power_state0', 'PMIC ligado', 1 ], [ 'power_state1', '5 V ligado', 1 ], [ 'power_state2', 'fontes boas', 1 ],
		[ 'power_state3', 'power_state3', null ], [ 'reset_state0', 'chips fora de reset', 1 ], [ 'reset_state1', 'PORESET solto', 1 ],
		[ 'reset_state2', 'reset_state2', 1 ], [ 'reset_state3', 'sequência completa', 1 ] ] ]
];

return view.extend({
	load: function() {
		return L.resolveDefault(callCpld(), {});
	},

	set: function(what, name, value, label) {
		return callCpldSet(what, name, value).then(L.bind(function(res) {
			if (!res || !res.ok)
				ui.addNotification(null, E('p', 'Não foi possível: ' + label + '.'), 'error');
			return this.refresh();
		}, this));
	},

	confirmAction: function(a) {
		ui.showModal(a[1], [
			E('p', {}, a[2]),
			E('p', {}, 'A placa reinicia agora e a página para de responder até ela voltar (uns 90 s).'),
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, 'Cancelar'), ' ',
				E('button', {
					'class': 'btn cbi-button-negative',
					'click': ui.createHandlerFn(this, function() {
						return callCpldSet('action', '', a[0]).then(function(res) {
							ui.hideModal();
							if (res && res.ok)
								ui.addNotification(null, E('p', a[1] + ' pedido. A placa está reiniciando.'), 'info');
						});
					})
				}, a[1])
			])
		]);
	},

	refresh: function() {
		return L.resolveDefault(callCpld(), {}).then(L.bind(function(d) {
			this.box.replaceChildren(this.renderAll(d));
		}, this));
	},

	renderAll: function(d) {
		var self = this;

		if (!d.v3)
			return E('div', { 'class': 'alert-message warning' }, [
				'Esta página precisa da imagem 3.0 da CPLD e do kmod-ls1021aiot-cpld novo. ',
				d.dip_switch ? 'Com a imagem atual só se lê as chaves SW2: ' + d.dip_switch + '.' : ''
			]);

		var st = d.status || {}, rs = d.resets || {}, ct = d.ctrl || {}, wd = d.watchdog || {};
		var btn = function(label, cls, fn) {
			return E('button', { 'class': 'btn cbi-button ' + (cls || ''), 'click': ui.createHandlerFn(self, fn) }, label);
		};
		var badge = function(on, yes, no, bad) {
			return E('span', { 'class': 'ls-level' + (on ? (bad ? ' ls-level-bad' : ' ls-level-hi') : '') }, on ? yes : no);
		};

		return E('div', {}, [
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr' }, [ E('td', { 'class': 'td left', 'width': '35%' }, 'Versão da CPLD'), E('td', { 'class': 'td' }, E('strong', {}, d.version)) ]),
				E('tr', { 'class': 'tr' }, [ E('td', { 'class': 'td left' }, 'Causa do último reset'),
					E('td', { 'class': 'td' }, [ CAUSES[d.cause.name] || d.cause.name, ' ', E('code', {}, d.cause.raw) ]) ]),
				E('tr', { 'class': 'tr' }, [ E('td', { 'class': 'td left' }, 'Chaves SW2'), E('td', { 'class': 'td' }, E('code', {}, d.dip_switch)) ])
			]),

			E('h3', {}, 'Reset dos chips'),
			E('p', { 'class': 'cbi-section-descr' }, 'Cada chip tem uma linha de reset na CPLD. "Reiniciar" segura o chip em reset por 10 ms; "Segurar" o deixa em reset até você soltar.'),
			E('div', { 'class': 'ls-scroll' }, E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, 'Chip'), E('th', { 'class': 'th' }, 'Sinal'), E('th', { 'class': 'th' }, 'Estado'),
					E('th', { 'class': 'th' }, ''), E('th', { 'class': 'th' }, 'O que acontece')
				])
			].concat(RESETS.map(function(r) {
				var held = rs[r[0]] == 1;
				return E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, r[1] ? E('a', { 'href': common.url(r[1]) }, r[2]) : r[2]),
					E('td', { 'class': 'td' }, E('code', {}, r[3])),
					E('td', { 'class': 'td' }, badge(held, 'em reset', 'rodando', true)),
					E('td', { 'class': 'td' }, E('div', { 'class': 'ls-toolbar', 'style': 'margin:0' }, [
						btn('Reiniciar', 'cbi-button-action', function() { return self.set('reset', r[0], 'pulse', 'reiniciar ' + r[2]); }),
						held ? btn('Soltar', 'cbi-button-positive', function() { return self.set('reset', r[0], '0', 'soltar ' + r[2]); })
						     : btn('Segurar', '', function() { return self.set('reset', r[0], '1', 'segurar ' + r[2]); })
					])),
					E('td', { 'class': 'td ls-muted' }, r[4])
				]);
			})))),

			E('h3', {}, 'Reiniciar a placa'),
			E('div', { 'class': 'ls-toolbar' }, ACTIONS.map(function(a) {
				return btn(a[1], 'cbi-button-negative', function() { self.confirmAction(a); });
			})),
			E('ul', { 'class': 'ls-muted' }, ACTIONS.map(function(a) { return E('li', {}, [ E('strong', {}, a[1] + ': '), a[2] ]); })),

			E('h3', {}, 'Watchdog'),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr' }, [ E('td', { 'class': 'td left', 'width': '35%' }, 'Estado'),
					E('td', { 'class': 'td' }, wd.enabled ? badge(true, 'ligado, ' + (wd.cold ? 'reset a frio' : 'reset a quente') + ' no fim', '') : badge(false, '', 'desligado')) ]),
				E('tr', { 'class': 'tr' }, [ E('td', { 'class': 'td left' }, 'Tempo limite'), E('td', { 'class': 'td' }, wd.timeout + ' s') ]),
				E('tr', { 'class': 'tr' }, [ E('td', { 'class': 'td left' }, 'Faltam'), E('td', { 'class': 'td' }, wd.enabled ? wd.left + ' s' : '-') ])
			]),
			E('p', { 'class': 'ls-muted' }, 'É a /dev/watchdog do sistema: o procd o liga no boot e o alimenta a cada 5 s. Se o sistema travar, a CPLD reinicia a placa. Ele desliga sozinho em todo reset.'),

			E('h3', {}, 'Controles'),
			E('table', { 'class': 'table' }, CTRLS.map(function(c) {
				var on = ct[c[0]] == 1;
				return E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td left', 'width': '35%' }, c[1]),
					E('td', { 'class': 'td' }, E('div', { 'class': 'ls-toolbar', 'style': 'margin:0' }, [
						btn('Não', on ? '' : 'cbi-button-positive', function() { return on ? self.set('ctrl', c[0], '0', c[1]) : null; }),
						btn('Sim', on ? 'cbi-button-positive' : '', function() { return on ? null : self.set('ctrl', c[0], '1', c[1]); })
					])),
					E('td', { 'class': 'td ls-muted' }, c[2])
				]);
			})),

			E('h3', {}, 'Linhas que a CPLD lê'),
			E('div', { 'class': 'ls-status' }, STATUS.map(function(g) {
				return E('div', { 'class': 'ls-status-group' }, [
					E('h4', {}, g[0]),
					E('div', { 'class': 'ls-pins' }, g[1].map(function(s) {
						var v = st[s[0]], cls = (s[2] == null) ? (v ? ' ls-on' : '') : (v == s[2] ? ' ls-ok' : ' ls-bad');
						return E('span', { 'class': 'ls-sig' + cls, 'title': s[0] + ' = ' + v }, [ s[1], E('small', {}, String(v)) ]);
					}))
				]);
			})),

			E('h3', {}, 'Registradores'),
			E('pre', { 'class': 'ls-regs' }, d.regs.trim().split('\n').map(function(l, i) {
				return ('0' + (i * 8).toString(16)).slice(-2) + ':  ' + l;
			}).join('\n')),
			E('p', { 'class': 'ls-muted' }, [ 'O mapa de cada registrador está no README do repositório da CPLD e na página ', E('a', { 'href': common.url('cpld') }, 'U33 CPLD'), '.' ])
		]);
	},

	render: function(d) {
		this.box = E('div', {}, this.renderAll(d));
		poll.add(L.bind(this.refresh, this), 2);

		return E('div', {}, [
			common.css,
			E('style', {}, [
				'.ls-level{display:inline-block;padding:2px 10px;border-radius:10px;background:#424242;color:#ddd}',
				'.ls-level-hi{background:#2e7d32;color:#fff}',
				'.ls-level-bad{background:#c62828;color:#fff}',
				'.ls-status{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:10px}',
				'.ls-status-group h4{margin:.4em 0}',
				'.ls-pins{display:flex;flex-wrap:wrap;gap:4px}',
				'.ls-sig{display:inline-flex;gap:6px;align-items:center;padding:2px 8px;border-radius:4px;border:1px solid #666;font-size:12px}',
				'.ls-sig small{opacity:.7}',
				'.ls-ok{background:#1b5e20;color:#fff;border-color:#2e7d32}',
				'.ls-bad{background:#b71c1c;color:#fff;border-color:#c62828}',
				'.ls-on{background:#37474f;color:#fff}',
				'.ls-regs{font-size:13px;padding:8px;background:#1b1b1b;color:#ddd;border-radius:6px;overflow-x:auto}'
			].join('')),
			E('h2', {}, 'CPLD'),
			E('p', { 'class': 'cbi-section-descr' }, [
				'A ', E('a', { 'href': common.url('cpld') }, 'CPLD de sistema (U33)'),
				' liga as fontes, solta os resets e lê o estado da placa. Com a imagem 3.0 ela tem registradores no IFC: ',
				'o reset de cada chip, os resets da placa, um watchdog, alguns controles e as linhas que ela enxerga. As E/S dela estão em ',
				E('a', { 'href': L.url('admin/status/ls1021aiot/io') }, 'E/S digitais'), '.'
			]),
			this.box
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
