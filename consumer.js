require('dotenv').config();          // lee las variables de .env
console.log('PGHOST=', process.env.PGHOST);
const mqtt = require('mqtt');
const { Pool } = require('pg');

// --- conexiones ------------------------------------------------------------
const pool   = new Pool();                // usa PGHOST, PGUSER, etc. del .env
const client = mqtt.connect(process.env.MQTT_URL);

client.on('connect', () => {
  console.log('MQTT conectado');
  client.subscribe('rfid/ingreso');
});

// --- lógica cada vez que llega un mensaje ----------------------------------
client.on('message', async (_topic, payload) => {
  try {
    const { uid, tipo, ts, esp32 } = JSON.parse(payload);

    // 1) profesor_id por tarjeta
    const { rows: prof } = await pool.query(
      'SELECT profesor_id FROM profesores WHERE tarjeta_id = $1',
      [uid]
    );
    if (!prof.length) {
      console.log(`UID sin asignar: ${uid}`);
      return;
    }

    // 2) aula_id por ESP32
    const { rows: aula } = await pool.query(
      'SELECT aula_id FROM aulas WHERE esp32_id = $1',
      [esp32]
    );
    if (!aula.length) {
      console.log(`ESP32 sin aula: ${esp32}`);
      return;
    }

    // 3) insertar movimiento
	console.log("🧪 ts recibido:", ts);
	console.log("🕒 Interpretado como milisegundos:", new Date(ts).toISOString());
	console.log("🕒 Interpretado como segundos:", new Date(ts * 1000).toISOString());

    await pool.query(
      `INSERT INTO ingresos (profesor_id, aula_id, evento, fecha)
       VALUES ($1, $2, $3, to_timestamp($4))`,
      [prof[0].profesor_id, aula[0].aula_id, tipo, ts]
    );

// Notificar cambio de estado por MQTT
client.publish(
  'profesores/estado',
  JSON.stringify({
    profesor_id: prof[0].profesor_id,
    evento: tipo,        // "entrada" o "salida"
    aula_id: aula[0].aula_id,
    timestamp: ts
  })
);

    console.log(`OK ${uid} → ${tipo} (aula ${aula[0].aula_id})`);
  } catch (err) {
    console.error('Error al procesar mensaje:', err.message);
  }
});

