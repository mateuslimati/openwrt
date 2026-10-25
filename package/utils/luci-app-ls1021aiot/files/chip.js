'use strict';
'require view';
'require poll';
'require rpc';
'require ui';
'require ls1021aiot.common as common';

/*
 * One component of the LS1021A-IoT: a close-up of it, where it is on the
 * board, what it does, how the processor talks to it and what it reads
 * right now (its own values, or those of the page it shares them with).
 * The page id is the last element of the path,
 * admin/status/ls1021aiot/chip/<id>.
 */

var callControl = rpc.declare({
	object: 'ls1021aiot',
	method: 'control',
	params: [ 'action', 'value' ]
});

/* the buttons of chips.json "control": the action of the backend's
 * control method and the values it takes */
var CONTROLS = {
	led0: [ [ 'off', 'Apagar' ], [ 'on', 'Acender' ], [ 'blink', 'Piscar' ] ]
};

function pageId() {
	var p = L.env.dispatchpath || L.env.requestpath;
	return p[p.length - 1];
}

return view.extend({
	load: function() {
		var id = pageId();
		return common.load().then(function(data) {
			var lid = common.liveId(data, id);
			data.id = id;
			data.liveId = lid;
			return Promise.all([ lid ? L.resolveDefault(common.callChip(lid), null) : null, common.closeupAvailable(id) ])
				.then(function(r) { data.live = r[0]; data.closeup = r[1]; return data; });
		});
	},

	render: function(data) {
		var id = data.id, c = data.chips.chips[id], order = data.chips.order, lid = data.liveId;
		var i = order.indexOf(id), live = E('div', {}, lid ? common.renderLive(data.live) : '');
		var stamp = E('span', { 'class': 'ls-muted' }, '');
		var zoomed = true, mapbox = E('div', {});
		var drawMap = function() {
			mapbox.innerHTML = '';
			mapbox.appendChild(E('a', { 'href': common.url(), 'title': 'Abrir o mapa da placa' },
				common.renderMap(data, { view: common.viewFor(data, id), highlight: id, zoom: zoomed })));
		};
		drawMap();

		if (!c)
			return E('p', {}, 'Componente desconhecido: ' + id);

		var refs = data.map.parts.filter(function(p) { return p.page == id; }).map(function(p) {
			return p.ref + (p.side == 'bot' ? ' (verso)' : '');
		});

		var refresh = function() {
			return common.callChip(lid).then(function(res) {
				live.innerHTML = '';
				live.appendChild(common.renderLive(res));
				stamp.textContent = 'atualizado às ' + new Date().toLocaleTimeString();
			});
		};
		if (lid) poll.add(refresh, 2);

		var nav = function(j, label) {
			return (j >= 0 && j < order.length)
				? E('a', { 'class': 'cbi-button', 'href': common.url(order[j]) }, label.format(data.chips.chips[order[j]].tab))
				: E('span');
		};

		return E('div', {}, [
			common.css,
			E('h2', {}, [ c.name ]),
			E('p', { 'class': 'cbi-section-descr' }, [ E('strong', {}, c.part) ]),
			E('div', { 'class': 'ls-chip' }, [
				E('div', {}, [
					data.closeup ? E('img', { 'class': 'ls-closeup', 'src': common.closeup(id), 'alt': c.tab }) : '',
					mapbox,
					E('div', { 'class': 'ls-toolbar', 'style': 'margin-top:.4em' }, [
						E('button', { 'class': 'cbi-button', 'click': function(ev) {
							zoomed = !zoomed;
							ev.target.textContent = zoomed ? 'Ver a placa inteira' : 'Aproximar';
							drawMap();
						} }, 'Ver a placa inteira'),
						E('span', { 'class': 'ls-muted' },
							refs.length ? 'Na placa: ' + refs.join(', ') + '.' : 'Na FRDM-KW41Z, encaixada nos headers Arduino.')
					])
				]),
				E('div', {}, [
					E('p', {}, E('strong', {}, c.summary)),
					E('p', {}, c.description),
					(c.bus && c.bus.length) ? E('h3', {}, 'Como o processador conversa com ele') : '',
					(c.bus && c.bus.length) ? E('table', { 'class': 'table' }, [
						E('tr', { 'class': 'tr table-titles' }, [
							E('th', { 'class': 'th' }, 'Barramento'),
							E('th', { 'class': 'th' }, 'Endereço'),
							E('th', { 'class': 'th' }, 'No Linux')
						])
					].concat((c.bus || []).map(function(b) {
						return E('tr', { 'class': 'tr' }, [
							E('td', { 'class': 'td' }, b.bus),
							E('td', { 'class': 'td' }, E('code', {}, b.addr)),
							E('td', { 'class': 'td' }, E('code', {}, b.linux))
						]);
					}))) : '',
					E('div', { 'class': 'ls-tags', 'style': 'margin-top:.6em' }, [ 'Protocolos: ' ].concat(
						(c.proto || []).map(function(p) { return E('span', {}, p); }))),
					(c.specs && c.specs.length) ? E('table', { 'class': 'table', 'style': 'margin-top:.6em' }, c.specs.map(function(s) {
						return E('tr', { 'class': 'tr' }, [
							E('td', { 'class': 'td left', 'width': '35%' }, s[0]),
							E('td', { 'class': 'td left' }, s[1])
						]);
					})) : '',
					c.notes ? E('div', { 'class': 'alert-message warning', 'style': 'margin-top:.8em' }, c.notes) : ''
				])
			]),
			CONTROLS[c.control] ? E('h3', {}, 'Controle') : '',
			CONTROLS[c.control] ? E('div', { 'class': 'ls-toolbar' }, CONTROLS[c.control].map(function(b) {
				return E('button', {
					'class': 'btn cbi-button',
					'click': ui.createHandlerFn(this, function() {
						return callControl(c.control, b[0]).then(refresh);
					})
				}, b[1]);
			}, this)) : '',
			lid ? E('h3', {}, [ 'Valores ao vivo ', stamp ]) : '',
			(lid && lid != id && data.chips.chips[lid]) ? E('p', { 'class': 'ls-muted' }, [
				'Este chip não tem leitura própria; estes são os valores de ',
				E('a', { 'href': common.url(lid) }, data.chips.chips[lid].tab), '.'
			]) : '',
			live,
			E('div', { 'class': 'ls-nav' }, [ nav(i - 1, '← %s'), nav(i + 1, '%s →') ])
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
