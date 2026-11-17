// ============================================================================
// LiXee-Box (fork SensorFusion) - agregation de sondes de temperature
//   #sensorFusion   Mesures -> SensorFusion : valeur fusionnee, sondes, reglages
// API : GET/POST /api/sensorfusion (components/webserver/web_sensorfusion.cpp)
// ============================================================================
(function() {
  'use strict';
  var A = window.LixeeApp;
  if (!A) return;
  var esc = A.escHtml;

  var devices = {};      // IEEE (majuscules) -> appareil Zigbee (alias, modele)
  var MODES = { avg: 'Moyenne', min: 'Minimum', max: 'Maximum' };

  function ago(s) {
    if (s == null) return '--';
    if (s < 60) return s + ' s';
    if (s < 3600) return Math.floor(s / 60) + ' min ' + (s % 60) + ' s';
    return Math.floor(s / 3600) + ' h ' + Math.floor((s % 3600) / 60) + ' min';
  }
  function fmtTemp(v) { return v == null ? '--' : Number(v).toFixed(2) + ' &deg;C'; }
  function devName(ieee) {
    var d = devices[ieee];
    var alias = d && String(d.alias || '').trim();
    return alias ? esc(alias) : esc(ieee);
  }
  function badge(cls, txt) { return '<span class="badge bg-' + cls + '">' + txt + '</span>'; }

  function pageSensorFusion() {
    A.setContent(
      '<div class="container py-4" style="max-width:960px">' +
        '<h4 class="mb-3">SensorFusion</h4>' +
        '<p class="text-muted small">Temp&eacute;rature agr&eacute;g&eacute;e de plusieurs sondes Zigbee ' +
          '(cluster 0x0402), publi&eacute;e en MQTT pour OpenFrisquetVisio. ' +
          'Le flux Home Assistant (ziwifi32/) n&rsquo;est pas modifi&eacute;.</p>' +
        '<div class="row g-3">' +
          '<div class="col-12 col-lg-5 d-flex"><div class="card p-3 flex-fill" id="sf-summary">' +
            '<div class="text-center p-3"><div class="spinner-border"></div></div></div></div>' +
          '<div class="col-12 col-lg-7 d-flex"><div class="card p-3 flex-fill">' +
            '<h6>Sondes</h6><div id="sf-probes"></div></div></div>' +
          '<div class="col-12"><div class="card p-3"><h6>Configuration</h6>' +
            '<form id="sf-form"><div class="text-muted">Chargement...</div></form></div></div>' +
        '</div>' +
      '</div>');

    $.getJSON('/api/zigbee/devices').always(function(devs) {
      devices = {};
      (Array.isArray(devs) ? devs : []).forEach(function(d) {
        if (d && d.ieee) devices[String(d.ieee).toUpperCase()] = d;
      });
      load(true);
      A.pageInterval(function() { load(false); }, 5000);
    });
  }

  function load(withForm) {
    $.getJSON('/api/sensorfusion').done(function(st) {
      renderSummary(st);
      renderProbes(st);
      if (withForm) renderForm(st.config || {});
    }).fail(function(xhr) {
      $('#sf-summary').html('<div class="text-danger">' + A.apiErrorMsg(xhr, 'API SensorFusion indisponible') + '</div>');
    });
  }

  function renderSummary(st) {
    var c = st.config || {};
    var total = (c.sensors || []).length;
    var h = '';
    if (!c.enabled) {
      h += '<div class="alert alert-secondary py-2 small mb-2">Fonction d&eacute;sactiv&eacute;e</div>';
    }
    h += '<div class="text-center my-2">' +
           '<div style="font-size:3rem;font-weight:300;line-height:1.1">' + fmtTemp(st.value) + '</div>' +
           '<div class="text-muted small">' + esc(MODES[c.mode] || c.mode) + ' de ' +
             st.validCount + ' / ' + total + ' sonde(s) valide(s)</div>' +
         '</div>';
    h += '<table class="table table-sm mb-0 small"><tbody>' +
      '<tr><td>MQTT</td><td>' + (st.mqttConnected ? badge('success', 'Connect&eacute;') : badge('danger', 'D&eacute;connect&eacute;')) + '</td></tr>' +
      '<tr><td>Topic</td><td><code>' + esc(c.topic) + '</code>' + (c.retain ? ' <span class="text-muted">(retain)</span>' : '') + '</td></tr>' +
      '<tr><td>Derni&egrave;re publication</td><td>' +
        (st.lastPublishAgo == null ? '<span class="text-muted">aucune depuis le d&eacute;marrage</span>'
                                   : fmtTemp(st.lastPublishValue) + ', il y a ' + ago(st.lastPublishAgo)) +
        (st.pending ? ' ' + badge('warning text-dark', 'en attente') : '') + '</td></tr>' +
      '<tr><td>Expiration</td><td>' + Math.round((c.timeout_s || 0) / 60) + ' min &middot; anti-rebond ' + c.debounce_s + ' s</td></tr>' +
      '</tbody></table>';
    $('#sf-summary').html(h);
  }

  function renderProbes(st) {
    var p = st.probes || [];
    if (!p.length) {
      $('#sf-probes').html('<p class="text-muted small mb-0">Aucune sonde configur&eacute;e : cochez-en dans la configuration ci-dessous.</p>');
      return;
    }
    var h = '<table class="table table-sm mb-0 small"><thead><tr><th>Sonde</th><th class="text-end">Temp.</th>' +
            '<th class="text-end">Mesure</th><th>&Eacute;tat</th></tr></thead><tbody>';
    p.forEach(function(x) {
      var state = x.valid ? badge('success', 'Valide')
                : (x.temp == null ? badge('secondary', 'Pas encore re&ccedil;ue') : badge('danger', 'Expir&eacute;e'));
      h += '<tr' + (x.valid ? '' : ' class="text-muted"') + '><td>' + devName(x.ieee) +
           '<div class="text-muted" style="font-size:.75em">' + esc(x.ieee) + '</div></td>' +
           '<td class="text-end">' + fmtTemp(x.temp) + '</td>' +
           '<td class="text-end">' + (x.ago == null ? '--' : 'il y a ' + ago(x.ago)) + '</td>' +
           '<td>' + state + '</td></tr>';
    });
    $('#sf-probes').html(h + '</tbody></table>' +
      '<div class="text-muted small mt-1">Les dur&eacute;es repartent de z&eacute;ro au red&eacute;marrage de la box.</div>');
  }

  function renderForm(c) {
    var sel = {};
    (c.sensors || []).forEach(function(s) { sel[String(s).toUpperCase()] = true; });
    // Appareils connus + sondes configurees absentes de la liste Zigbee
    var list = Object.keys(devices);
    Object.keys(sel).forEach(function(s) { if (list.indexOf(s) < 0) list.push(s); });
    // Capteurs de temperature (deviceId 0x0302) en premier
    list.sort(function(a, b) {
      var ta = devices[a] && devices[a].deviceId === 0x0302 ? 0 : 1;
      var tb = devices[b] && devices[b].deviceId === 0x0302 ? 0 : 1;
      return ta - tb || devName(a).localeCompare(devName(b));
    });

    var h = '<div class="row g-3">' +
      '<div class="col-12 col-md-6">' +
        '<div class="form-check form-switch mb-2"><input class="form-check-input" type="checkbox" id="sf-enabled"' + (c.enabled ? ' checked' : '') + '>' +
          '<label class="form-check-label" for="sf-enabled">Activ&eacute;</label></div>' +
        '<label class="form-label small mb-0" for="sf-mode">Calcul</label>' +
        '<select class="form-select form-select-sm mb-2" id="sf-mode">' +
          Object.keys(MODES).map(function(m) {
            return '<option value="' + m + '"' + (c.mode === m ? ' selected' : '') + '>' + MODES[m] + '</option>';
          }).join('') + '</select>' +
        '<label class="form-label small mb-0" for="sf-topic">Topic MQTT</label>' +
        '<input class="form-control form-control-sm mb-2" id="sf-topic" maxlength="127" value="' + esc(c.topic) + '">' +
        '<div class="row g-2">' +
          '<div class="col-6"><label class="form-label small mb-0" for="sf-timeout">Expiration (min)</label>' +
            '<input type="number" class="form-control form-control-sm" id="sf-timeout" min="1" max="1440" value="' + Math.round((c.timeout_s || 2700) / 60) + '"></div>' +
          '<div class="col-6"><label class="form-label small mb-0" for="sf-debounce">Anti-rebond (s)</label>' +
            '<input type="number" class="form-control form-control-sm" id="sf-debounce" min="10" max="3600" value="' + (c.debounce_s || 10) + '"></div>' +
        '</div>' +
        '<div class="form-check form-switch mt-2"><input class="form-check-input" type="checkbox" id="sf-retain"' + (c.retain ? ' checked' : '') + '>' +
          '<label class="form-check-label" for="sf-retain">Message conserv&eacute; par le broker (retain)</label></div>' +
      '</div>' +
      '<div class="col-12 col-md-6"><div class="small mb-1">Sondes agr&eacute;g&eacute;es</div>';
    if (!list.length) h += '<div class="text-muted small">Aucun appareil Zigbee.</div>';
    list.forEach(function(ieee, i) {
      var d = devices[ieee];
      var info = d ? esc((d.manufacturer || '') + ' ' + (d.model || '')).trim() : 'appareil inconnu';
      h += '<div class="form-check"><input class="form-check-input sf-sensor" type="checkbox" id="sf-s' + i + '" value="' + esc(ieee) + '"' + (sel[ieee] ? ' checked' : '') + '>' +
           '<label class="form-check-label small" for="sf-s' + i + '">' + devName(ieee) +
           ' <span class="text-muted">' + info + '</span></label></div>';
    });
    h += '</div></div>' +
      '<div class="text-danger small mt-2" id="sf-error" style="display:none"></div>' +
      '<button type="submit" class="btn btn-primary btn-sm mt-3">Enregistrer</button>';
    $('#sf-form').html(h).off('submit').on('submit', save);
  }

  function save(ev) {
    ev.preventDefault();
    var body = {
      enabled: $('#sf-enabled').is(':checked'),
      mode: $('#sf-mode').val(),
      topic: $.trim($('#sf-topic').val()),
      timeout_s: Math.round(parseFloat($('#sf-timeout').val()) * 60),
      debounce_s: parseInt($('#sf-debounce').val(), 10),
      retain: $('#sf-retain').is(':checked'),
      sensors: $('.sf-sensor:checked').map(function() { return this.value; }).get()
    };
    $('#sf-error').hide();
    A.postJSON('/api/sensorfusion', body).done(function() {
      A.showAlert('Configuration SensorFusion enregistr&eacute;e');
      setTimeout(function() { load(false); }, 1500);   // appliquee par la boucle main (~1 s)
    }).fail(function(xhr) {
      $('#sf-error').html(A.apiErrorMsg(xhr, 'Enregistrement impossible')).show();
    });
  }

  A.registerPage('sensorFusion', pageSensorFusion);
})();
