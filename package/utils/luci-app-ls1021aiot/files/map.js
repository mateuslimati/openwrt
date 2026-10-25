'use strict';
'require view';
'require ls1021aiot.common as common';

/*
 * LS1021A-IoT board map: the photo of the board (or the drawing from its
 * layout) with every part that has a page outlined. Hovering a part shows
 * what it is and its live values; clicking opens its page.
 */

var VIEW_KEY = 'ls1021aiot.map.view';

function savedView(data) {
	var v;
	try { v = localStorage.getItem(VIEW_KEY); } catch (e) {}
	return data.photos[v] || v == 'layout' ? v : (data.photo ? 'photo' : 'layout');
}

function saveView(v) {
	try { localStorage.setItem(VIEW_KEY, v); } catch (e) {}
}

return view.extend({
	load: function() {
		return common.load();
	},

	tooltip: function() {
		if (!this.tip) {
			this.tip = E('div', { 'class': 'ls-tip', 'style': 'display:none' });
			document.body.appendChild(this.tip);
		}
		return this.tip;
	},

	placeTip: function(ev) {
		var tip = this.tooltip(), x = ev.clientX + 16, y = ev.clientY + 16;
		if (x + tip.offsetWidth > window.innerWidth - 8)
			x = ev.clientX - tip.offsetWidth - 16;
		if (y + tip.offsetHeight > window.innerHeight - 8)
			y = ev.clientY - tip.offsetHeight - 16;
		tip.style.left = Math.max(8, x) + 'px';
		tip.style.top = Math.max(8, y) + 'px';
	},

	showTip: function(data, id, ev) {
		var c = data.chips.chips[id], tip = this.tooltip(), live = E('div', { 'class': 'ls-tip-live ls-muted' }, 'lendo…');
		var token = this.hovered = id, lid = common.liveId(data, id), img = E('img', { 'alt': '' });

		tip.innerHTML = '';
		tip.appendChild(E('div', {}, [ E('b', {}, c.tab), ' — ', c.name ]));
		img.onerror = function() { img.remove(); };
		img.onload = L.bind(function() { this.placeTip(ev); }, this);
		img.src = common.closeup(id);
		tip.appendChild(img);
		tip.appendChild(E('div', {}, c.summary));
		if (lid)
			tip.appendChild(live);
		tip.style.display = '';
		this.placeTip(ev);

		if (!lid)
			return;

		/* a few live values: the first ones the page shows */
		common.callChip(lid).then(L.bind(function(res) {
			if (this.hovered != token)
				return;
			var vals = (res && res.values || []).slice(0, 4);
			live.className = 'ls-tip-live';
			live.innerHTML = '';
			if (!vals.length && res && res.tables && res.tables.length)
				live.appendChild(E('div', {}, res.tables[0].title + ': ' + res.tables[0].rows.length + ' linhas'));
			vals.forEach(function(v) {
				live.appendChild(E('div', {}, [ v.label + ': ', E('b', {}, v.value), v.unit ? ' ' + v.unit : '' ]));
			});
		}, this));
	},

	hideTip: function() {
		this.hovered = null;
		if (this.tip)
			this.tip.style.display = 'none';
	},

	renderInto: function(box, data, which) {
		var self = this;
		box.innerHTML = '';
		box.appendChild(common.renderMap(data, {
			view: which,
			onhover: function(id, ev) { self.showTip(data, id, ev); },
			onmove: function(ev) { self.placeTip(ev); },
			onleave: function() { self.hideTip(); }
		}));
	},

	render: function(data) {
		var self = this, which = savedView(data);
		var box = E('div', { 'class': 'ls-mapbox' });

		var btn = function(v, label) {
			return E('button', {
				'class': 'cbi-button' + (which == v ? ' cbi-button-action' : ''),
				'disabled': (v != 'layout' && !data.photos[v]) ? '' : null,
				'click': function(ev) {
					which = v;
					saveView(v);
					ev.target.parentNode.querySelectorAll('button').forEach(function(b) {
						b.classList.toggle('cbi-button-action', b === ev.target);
					});
					self.renderInto(box, data, v);
				}
			}, label);
		};

		this.renderInto(box, data, which);

		return E('div', {}, [
			common.css,
			E('h2', {}, 'Mapa da placa'),
			E('p', { 'class': 'cbi-section-descr' },
				'Passe o mouse sobre um componente para ver o que ele é e o que está medindo agora. Clique para abrir a página dele. ' +
				'Nas vistas de cima, os contornos tracejados em azul estão no verso da placa; a vista do verso mostra a placa virada, vista por baixo. ' +
				'A FRDM-KW41Z, quando encaixada, cobre a área tracejada em roxo; na foto com ela, os chips dela aparecem no lugar.'),
			E('div', { 'class': 'ls-toolbar' }, [
				btn('photo', 'Foto'),
				btn('frdm', 'Com a FRDM-KW41Z'),
				btn('bottom', 'Verso'),
				btn('layout', 'Desenho do layout'),
				data.photo ? '' : E('span', { 'class': 'ls-muted' }, 'As fotos não estão instaladas (/www/luci-static/ls1021aiot/); mostrando o desenho do layout.')
			]),
			box,
			E('h3', { 'style': 'margin-top:1em' }, 'Todos os componentes'),
			common.renderGrid(data)
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
