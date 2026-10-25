'use strict';
'require view';
'require ls1021aiot.common as common';

/* LS1021A-IoT components: one card per page, by group */

return view.extend({
	load: function() {
		return common.load();
	},

	render: function(data) {
		return E('div', {}, [
			common.css,
			E('h2', {}, 'Componentes'),
			E('p', { 'class': 'cbi-section-descr' }, [
				'Cada componente tem uma página com o que ele faz, como o processador conversa com ele e o que ele mede agora. ',
				'Para achá-los na placa, use o ', E('a', { 'href': common.url() }, 'mapa da placa'), '.'
			]),
			common.renderGrid(data)
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
