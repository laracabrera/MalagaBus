/**
 * Pebble application with information about EMT Malaga buses
 */

var UI = require('ui');
var ajax = require('ajax');

var main = new UI.Card({
  title: 'Pebble.js',
  icon: 'images/menu_icon.png',
  subtitle: 'Hello World!',
  body: 'Press any button.',
  subtitleColor: 'indigo', // Named colors
  bodyColor: '#9a0036' // Hex colors
});

main.show();


var codParada = 2304;
ajax(
    {
      url: 'http://www.emtmalaga.es/emt-mobile/informacionParada.html?codParada=' + codParada
    },
    function(data, status, request) {
      console.log('Informacion obtenida: ' + data);
    },
    function(error, status, request) {
      console.log('The ajax request failed: ' + error);
    }
);
