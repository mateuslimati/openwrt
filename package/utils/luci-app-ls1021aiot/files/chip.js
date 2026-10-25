'use strict';
'require view';
'require poll';
'require ls1021aiot.common as common';

/*
 * One component of the LS1021A-IoT: where it is on the board, what it
 * does, how the processor talks to it and what it reads right now. The
 * page id is the last element of the path, admin/status/ls1021aiot/chip/<id>.
 */

function pageId() {
	var p = L.env.dispatchpath || L.env.requestpath;
	return p[p.length - 1];
}

return view.extend({
	load: function() {
		var id = pageId();
		return Promise.all([ common.load(), L.resolveDefault(common.callChip(id), null) ])
			.then(function(r) { r[0].id = id; r[0].live = r[1]; return r[0]; });
	},

	render: function(data) {
		var id = data.id, c = data.chips.chips[id], order = data.chips.order;
		var i = order.indexOf(id), live = E('div', {}, common.renderLive(data.live));
		var stamp = E('span', { 'class': 'ls-muted' }, '');
		var zoomed = true, mapbox = E('div', {});
		var drawMap = function() {
			mapbox.innerHTML = '';
			mapbox.appendChild(E('a', { 'href': common.url(), 'title': 'Abrir o mapa da placa' },
				common.renderMap(data, { view: data.photo ? 'photo' : 'layout', highlight: id, zoom: zoomed })));
		};
		drawMap();

		if (!c)
			return E('p', {}, 'Componente desconhecido: ' + id);

		var refs = data.map.parts.filter(function(p) { return p.page == id; }).map(function(p) {
			return p.ref + (p.side == 'bot' ? ' (verso)' : '');
		});

		poll.add(function() {
			return common.callChip(id).then(function(res) {
				live.innerHTML = '';
				live.appendChild(common.renderLive(res));
				stamp.textContent = 'atualizado às ' + new Date().toLocaleTimeString();
			});
		}, 2);

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
					E('h3', {}, 'Como o processador conversa com ele'),
					E('table', { 'class': 'table' }, [
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
					}))),
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
			E('h3', {}, [ 'Valores ao vivo ', stamp ]),
			live,
			E('div', { 'class': 'ls-nav' }, [ nav(i - 1, '← %s'), nav(i + 1, '%s →') ])
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
