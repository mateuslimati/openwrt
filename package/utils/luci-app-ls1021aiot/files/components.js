'use strict';
'require view';
'require ls1021aiot.common as common';

/* LS1021A-IoT components: one card per page, in the order of the tabs */

return view.extend({
	load: function() {
		return common.load();
	},

	render: function(data) {
		var chips = data.chips.chips;

		return E('div', {}, [
			common.css,
			E('h2', {}, 'Componentes'),
			E('p', { 'class': 'cbi-section-descr' }, [
				'Cada componente tem uma página com o que ele faz, como o processador conversa com ele e o que ele mede agora. ',
				'Para achá-los na placa, use o ', E('a', { 'href': common.url() }, 'mapa da placa'), '.'
			]),
			E('div', { 'class': 'ls-grid' }, data.chips.order.map(function(id) {
				var c = chips[id];
				return E('a', { 'class': 'ls-card', 'href': common.url(id) }, [
					E('strong', {}, c.tab), ' ', c.name,
					E('small', {}, c.summary),
					E('small', { 'class': 'ls-tags' }, (c.bus || []).slice(0, 1).map(function(b) {
						return E('span', {}, b.bus + ' · ' + b.addr);
					}))
				]);
			}))
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
