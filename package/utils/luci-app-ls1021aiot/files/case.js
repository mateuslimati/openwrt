'use strict';
'require view';
'require request';
'require ls1021aiot.common as common';

/*
 * The two panels of the LS1021A-IoT enclosure: every connector, LED and
 * button is a spot that tells where it goes on the board and opens the
 * page of the part behind it.
 */

var SVGNS = 'http://www.w3.org/2000/svg';
var PANELS = [
	[ 'front', 'Frente', 'Rede, USB, HDMI, cartão SD e os LEDs.' ],
	[ 'rear', 'Traseira', 'Energia, áudio, as portas de expansão e a abertura da FRDM-KW41Z.' ]
];

function svgEl(name, attrs, children) {
	var el = document.createElementNS(SVGNS, name);
	for (var k in attrs || {})
		el.setAttribute(k, attrs[k]);
	(children || []).forEach(function(c) {
		el.appendChild(typeof c == 'string' ? document.createTextNode(c) : c);
	});
	return el;
}

function photo(name) {
	return L.resource('../ls1021aiot/case-' + name + '.jpg');
}

function available(url) {
	return new Promise(function(resolve) {
		var img = new Image();
		img.onload = function() { resolve(true); };
		img.onerror = function() { resolve(false); };
		img.src = url;
	});
}

return view.extend({
	load: function() {
		return Promise.all([
			common.load(),
			request.get(L.resource('ls1021aiot/panels.json')).then(function(res) { return res.json(); }),
			available(photo('front')), available(photo('rear'))
		]).then(function(r) {
			r[0].panels = r[1];
			r[0].casePhotos = { front: r[2], rear: r[3] };
			return r[0];
		});
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

	showTip: function(data, s, ev) {
		var tip = this.tooltip(), c = s.page ? data.chips.chips[s.page] : null;
		tip.innerHTML = '';
		tip.appendChild(E('div', {}, E('b', {}, s.label)));
		tip.appendChild(E('div', {}, s.text));
		if (c)
			tip.appendChild(E('div', { 'class': 'ls-tip-live' }, [ 'Clique para abrir: ', E('b', {}, c.tab) ]));
		tip.style.display = '';
		this.placeTip(ev);
	},

	hideTip: function() {
		if (this.tip)
			this.tip.style.display = 'none';
	},

	renderPanel: function(data, name) {
		var self = this, p = data.panels[name];
		var svg = svgEl('svg', {
			'viewBox': '0 0 ' + p.w + ' ' + p.h,
			'class': 'ls-map ls-case',
			'role': 'img',
			'aria-label': 'Painel do gabinete'
		}, [ svgEl('image', { 'href': photo(name), 'x': 0, 'y': 0, 'width': p.w, 'height': p.h }) ]);

		p.spots.forEach(function(s) {
			var shape = s.rect
				? svgEl('rect', { 'x': s.rect[0], 'y': s.rect[1], 'width': s.rect[2], 'height': s.rect[3], 'rx': 4 })
				: svgEl('circle', { 'cx': s.circle[0], 'cy': s.circle[1], 'r': s.circle[2] });
			var el = svgEl(s.page ? 'a' : 'g', s.page
				? { 'href': common.url(s.page), 'class': 'ls-part' }
				: { 'class': 'ls-part ls-nolink' }, [ shape, svgEl('title', {}, [ s.label ]) ]);
			el.addEventListener('mouseenter', function(ev) { self.showTip(data, s, ev); });
			el.addEventListener('mousemove', function(ev) { self.placeTip(ev); });
			el.addEventListener('mouseleave', function() { self.hideTip(); });
			svg.appendChild(el);
		});
		return svg;
	},

	render: function(data) {
		var self = this;

		return E('div', {}, [
			common.css,
			E('style', {}, [
				'.ls-case{background:#222}',
				'.ls-case .ls-part rect,.ls-case .ls-part circle{fill:rgba(255,193,7,.06);stroke:#ffc107;stroke-width:2;vector-effect:non-scaling-stroke;cursor:pointer;transition:fill .15s}',
				'.ls-case .ls-part:hover rect,.ls-case .ls-part:hover circle{fill:rgba(255,193,7,.38);stroke-width:3}',
				'.ls-case .ls-nolink rect,.ls-case .ls-nolink circle{stroke:#9e9e9e;stroke-dasharray:4 3;cursor:help}',
				'.ls-case .ls-nolink:hover circle{fill:rgba(158,158,158,.3)}'
			].join('')),
			E('h2', {}, 'Gabinete'),
			E('p', { 'class': 'cbi-section-descr' },
				'Os conectores, LEDs e botões dos dois painéis. Passe o mouse para ver aonde cada um vai na placa; clique para abrir a página da peça por trás dele. ' +
				'Os furos das antenas (em cinza) não têm peça na placa.'),
			E('div', {}, PANELS.map(function(pn) {
				var p = data.panels[pn[0]];
				return E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, pn[1]),
					E('p', { 'class': 'ls-muted' }, pn[2]),
					data.casePhotos[pn[0]] ? self.renderPanel(data, pn[0])
						: E('p', { 'class': 'ls-muted' }, 'A foto deste painel não está instalada (/www/luci-static/ls1021aiot/case-' + pn[0] + '.jpg).'),
					E('div', { 'class': 'ls-scroll', 'style': 'margin-top:.6em' }, E('table', { 'class': 'table' }, [
						E('tr', { 'class': 'tr table-titles' }, [
							E('th', { 'class': 'th' }, 'No painel'),
							E('th', { 'class': 'th' }, 'O que é'),
							E('th', { 'class': 'th' }, 'Página')
						])
					].concat(p.spots.filter(function(s) { return s.page; }).map(function(s) {
						var c = data.chips.chips[s.page];
						return E('tr', { 'class': 'tr' }, [
							E('td', { 'class': 'td', 'style': 'white-space:nowrap' }, E('strong', {}, s.label)),
							E('td', { 'class': 'td left' }, s.text),
							E('td', { 'class': 'td', 'style': 'white-space:nowrap' }, E('a', { 'href': common.url(s.page) }, c.tab))
						]);
					}))))
				]);
			}))
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
