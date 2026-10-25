'use strict';
'require baseclass';
'require request';
'require rpc';

/*
 * Shared by the board map and the component pages: the component data
 * (chips.json), the outline of each part on the photo and on the layout
 * drawing (map.json), the clickable map itself and the rendering of the
 * live values that ls1021aiot-chip returns.
 */

var BASE = 'admin/status/ls1021aiot';
var PHOTO = L.resource('../ls1021aiot/board.jpg');
var LAYOUT = L.resource('ls1021aiot/layout.svg');
var SVGNS = 'http://www.w3.org/2000/svg';

var callChip = rpc.declare({
	object: 'ls1021aiot',
	method: 'chip',
	params: [ 'id' ]
});

var cache = {};

function fetchJSON(name) {
	if (!cache[name])
		cache[name] = request.get(L.resource('ls1021aiot/' + name)).then(function(res) {
			return res.json();
		});
	return cache[name];
}

/* the photo is installed on the board only; without it, the drawing */
function photoAvailable() {
	if (!cache.photo)
		cache.photo = new Promise(function(resolve) {
			var img = new Image();
			img.onload = function() { resolve(true); };
			img.onerror = function() { resolve(false); };
			img.src = PHOTO;
		});
	return cache.photo;
}

function svgEl(name, attrs, children) {
	var el = document.createElementNS(SVGNS, name);
	for (var k in attrs || {})
		el.setAttribute(k, attrs[k]);
	(children || []).forEach(function(c) {
		el.appendChild(typeof c == 'string' ? document.createTextNode(c) : c);
	});
	return el;
}

function points(pts) {
	return pts.map(function(p) { return p[0] + ',' + p[1]; }).join(' ');
}

function bbox(pts) {
	var xs = pts.map(function(p) { return p[0]; }), ys = pts.map(function(p) { return p[1]; });
	return [ Math.min.apply(null, xs), Math.min.apply(null, ys), Math.max.apply(null, xs), Math.max.apply(null, ys) ];
}

/* a 4:3 window around the parts, at least min wide, inside the image */
function zoomBox(pts, dim, min) {
	var b = bbox(pts), w = Math.max(min, (b[2] - b[0]) * 2.2), h, x, y;
	h = Math.max(w * 0.75, (b[3] - b[1]) * 2.2);
	w = Math.max(w, h / 0.75);
	w = Math.min(w, dim.w); h = Math.min(h, dim.h);
	x = Math.min(Math.max(0, (b[0] + b[2]) / 2 - w / 2), dim.w - w);
	y = Math.min(Math.max(0, (b[1] + b[3]) / 2 - h / 2), dim.h - h);
	return [ x, y, w, h ].map(function(v) { return Math.round(v); }).join(' ');
}

function centre(pts) {
	var x = 0, y = 0;
	pts.forEach(function(p) { x += p[0]; y += p[1]; });
	return [ x / pts.length, y / pts.length ];
}

return baseclass.extend({
	BASE: BASE,
	callChip: callChip,

	load: function() {
		return Promise.all([ fetchJSON('chips.json'), fetchJSON('map.json'), photoAvailable() ])
			.then(function(r) { return { chips: r[0], map: r[1], photo: r[2] }; });
	},

	url: function(id) {
		return id ? L.url(BASE, 'chip', id) : L.url(BASE, 'map');
	},

	/*
	 * The board image with an outline over every part that has a page.
	 * opts.view: 'photo' or 'layout'; opts.highlight: page id to mark;
	 * opts.zoom: show only the area around it; opts.onhover(id, event)
	 * and opts.onleave() for a tooltip.
	 */
	renderMap: function(data, opts) {
		var self = this, useLayout = opts.view == 'layout' || !data.photo;
		var dim = useLayout ? data.map.layout : data.map.photo;
		var key = useLayout ? 'layout' : 'photo';
		var svg = svgEl('svg', {
			'viewBox': '0 0 ' + dim.w + ' ' + dim.h,
			'class': 'ls-map' + (opts.highlight ? ' ls-map-focus' : ''),
			'role': 'img',
			'aria-label': 'Placa LS1021A-IoT'
		});

		svg.appendChild(svgEl('image', {
			'href': useLayout ? LAYOUT : PHOTO,
			'x': 0, 'y': 0, 'width': dim.w, 'height': dim.h
		}));

		/* the FRDM-KW41Z, when plugged, covers the Arduino headers */
		var f = data.map.frdm[key], fb = bbox(f), fc = [ fb[0], fb[1] ];
		var frdm = svgEl('g', { 'class': 'ls-frdm' }, [
			svgEl('polygon', { 'points': points(f) }),
			svgEl('text', { 'x': fc[0] + 6, 'y': fc[1] + 20 }, [ 'FRDM-KW41Z (encaixada)' ])
		]);
		[ 'kw41z', 'fxos8700' ].forEach(function(id, i) {
			var c = data.chips.chips[id];
			var a = svgEl('a', { 'href': self.url(id), 'class': 'ls-pill' + (opts.highlight == id ? ' active' : '') }, [
				svgEl('rect', { 'x': fc[0] + 6 + i * 96, 'y': fc[1] + 30, 'width': 90, 'height': 22, 'rx': 11 }),
				svgEl('text', { 'x': fc[0] + 51 + i * 96, 'y': fc[1] + 45 }, [ i ? 'FXOS8700CQ' : 'KW41Z (BLE)' ])
			]);
			a.appendChild(svgEl('title', {}, [ c.name ]));
			self.bindHover(a, id, opts);
			frdm.appendChild(a);
		});
		svg.appendChild(frdm);

		data.map.parts.forEach(function(p) {
			var c = data.chips.chips[p.page];
			var cls = 'ls-part' + (p.side == 'bot' ? ' ls-bottom' : '') +
				(opts.highlight == p.page ? ' active' : '');
			var a = svgEl('a', { 'href': self.url(p.page), 'class': cls }, [
				svgEl('polygon', { 'points': points(p[key]) }),
				svgEl('title', {}, [ p.ref + ' — ' + c.name + (p.side == 'bot' ? ' (verso)' : '') ])
			]);
			if (opts.highlight == p.page) {
				var m = centre(p[key]);
				a.appendChild(svgEl('circle', { 'class': 'ls-pulse', 'cx': m[0], 'cy': m[1], 'r': 6 }));
			}
			self.bindHover(a, p.page, opts);
			svg.appendChild(a);
		});

		if (opts.zoom && opts.highlight) {
			var pts = [];
			data.map.parts.forEach(function(p) {
				if (p.page == opts.highlight)
					pts = pts.concat(p[key]);
			});
			if (!pts.length)
				pts = f;
			svg.setAttribute('viewBox', zoomBox(pts, dim, dim.w * 0.3));
		}

		return svg;
	},

	bindHover: function(el, id, opts) {
		if (!opts.onhover)
			return;
		el.addEventListener('mouseenter', function(ev) { opts.onhover(id, ev); });
		el.addEventListener('mousemove', function(ev) { opts.onmove && opts.onmove(ev); });
		el.addEventListener('mouseleave', function() { opts.onleave && opts.onleave(); });
	},

	/* the values, tables and text that ls1021aiot-chip returns */
	renderLive: function(res) {
		var nodes = [];

		if (!res || (!res.values && !res.tables))
			return E('p', { 'class': 'ls-muted' }, 'Sem resposta da placa.');

		if (res.values && res.values.length)
			nodes.push(E('table', { 'class': 'table ls-values' }, res.values.map(function(v) {
				return E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td left', 'width': '45%' }, v.label),
					E('td', { 'class': 'td left' }, [
						E('strong', {}, v.value), v.unit ? ' ' + v.unit : ''
					])
				]);
			})));

		(res.tables || []).forEach(function(t) {
			nodes.push(E('h4', {}, t.title));
			nodes.push(E('div', { 'class': 'ls-scroll' }, E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, t.head.map(function(h) {
					return E('th', { 'class': 'th' }, h);
				}))
			].concat(t.rows.map(function(r) {
				return E('tr', { 'class': 'tr' }, r.map(function(c) {
					return E('td', { 'class': 'td' }, c);
				}));
			})))));
		});

		if (res.text && res.text.length)
			nodes.push(E('pre', { 'class': 'ls-pre' }, res.text.join('\n')));

		return E('div', {}, nodes);
	},

	css: E('style', {}, [
		'.ls-map{width:100%;height:auto;display:block;border-radius:6px;background:#0d5c33}',
		'.ls-part polygon{fill:rgba(255,193,7,.06);stroke:#ffc107;stroke-width:2;vector-effect:non-scaling-stroke;cursor:pointer;transition:fill .15s}',
		'.ls-part:hover polygon,.ls-part:focus polygon{fill:rgba(255,193,7,.38);stroke-width:3}',
		'.ls-part.ls-bottom polygon{stroke:#4fc3f7;stroke-dasharray:6 4;fill:rgba(79,195,247,.06)}',
		'.ls-part.ls-bottom:hover polygon{fill:rgba(79,195,247,.35)}',
		'.ls-map-focus .ls-part:not(.active) polygon{stroke-opacity:.35}',
		'.ls-part.active polygon{fill:rgba(255,64,129,.08);stroke:#ff4081;stroke-width:3}',
		'.ls-pulse{fill:none;stroke:#ff4081;stroke-width:2;vector-effect:non-scaling-stroke;animation:lspulse 1.4s ease-out infinite;transform-box:fill-box;transform-origin:center}',
		'@keyframes lspulse{0%{opacity:.9;transform:scale(1)}100%{opacity:0;transform:scale(5)}}',
		'.ls-frdm polygon{fill:rgba(224,64,251,.05);stroke:#e040fb;stroke-width:2;stroke-dasharray:10 6;vector-effect:non-scaling-stroke;pointer-events:none}',
		'.ls-frdm>text{fill:#e040fb;font:bold 15px sans-serif;paint-order:stroke;stroke:#000;stroke-width:3px;pointer-events:none}',
		'.ls-pill rect{fill:#e040fb;stroke:#fff;stroke-width:1.5;cursor:pointer}',
		'.ls-pill:hover rect,.ls-pill.active rect{fill:#ff4081}',
		'.ls-pill text{fill:#fff;font:bold 12px sans-serif;text-anchor:middle;pointer-events:none}',
		'.ls-tip{position:fixed;z-index:1000;max-width:320px;padding:.6em .8em;border-radius:6px;background:rgba(20,20,20,.94);color:#eee;font-size:90%;pointer-events:none;box-shadow:0 4px 14px rgba(0,0,0,.4)}',
		'.ls-tip b{color:#ffc107}.ls-tip .ls-tip-live{margin-top:.4em;border-top:1px solid #444;padding-top:.4em}',
		'.ls-muted{opacity:.7}',
		'.ls-scroll{overflow-x:auto}',
		'.ls-pre{font-size:85%;overflow-x:auto;padding:.6em;border-radius:4px;background:rgba(127,127,127,.12)}',
		'.ls-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:.8em}',
		'.ls-card{display:block;padding:.8em;border:1px solid rgba(127,127,127,.35);border-radius:6px;color:inherit;text-decoration:none}',
		'.ls-card:hover{border-color:#ffc107}',
		'.ls-card small{display:block;opacity:.75;margin-top:.3em}',
		'.ls-tags span{display:inline-block;margin:0 .3em .3em 0;padding:.1em .55em;border-radius:1em;background:rgba(127,127,127,.18);font-size:85%}',
		'.ls-chip{display:grid;grid-template-columns:minmax(260px,2fr) 3fr;gap:1.2em;align-items:start}',
		'@media (max-width:800px){.ls-chip{grid-template-columns:1fr}}',
		'.ls-toolbar{display:flex;gap:.5em;align-items:center;flex-wrap:wrap;margin-bottom:.6em}',
		'.ls-nav{display:flex;justify-content:space-between;margin-top:1em}'
	].join(''))
});
