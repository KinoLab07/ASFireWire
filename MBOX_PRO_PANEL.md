# El panel de control de la Mbox Pro

> **Nota del 21 de septiembre de 2026 — el panel de control queda abandonado.**
> Este documento se conserva íntegro porque es el material del artículo y porque
> las mediciones del aparato que contiene siguen siendo válidas. Pero **su
> sección 10 ya no es la lista de trabajo**: los pendientes de interfaz están
> cancelados. La lista viva está en `ESTADO-Y-PENDIENTES.md`.


Reconstrucción del panel de control de Avid para la Mbox Pro (3ª gen, 2011) sobre
macOS Tahoe, sin nada de Avid instalado. Documento de la sesión del **18 de
septiembre de 2026**, en la que se pasó de *"nada funciona"* a la interfaz
mandando sobre el aparato de punta a punta.

El driver original de Avid se detiene en Mojave y Apple retiró la pila FireWire
del sistema. El aparato ya sonaba desde ASFireWire; lo que faltaba era el
control: faders, paneos, medidores y enlaces estéreo.

> Los gráficos originales de Avid se usan **solo como referencia**, viven fuera
> del repositorio y no se distribuyen. Los reemplazos serán diseños propios
> derivados de la función, no recoloreados del arte de Avid.

---

## 1. El error de fondo: el router estaba leído al revés

Durante buena parte del trabajo previo, las entradas del router TCAT se
documentaron como `(destino << 8) | fuente`. **Es al revés**: el byte alto es la
**fuente** y el bajo el **destino**.

Esa inversión contaminó todo el mapa del aparato y produjo al menos tres
conclusiones falsas que costaron días, entre ellas la de que el bloque 3
alimentaba el auricular A.

### Cómo se zanjó

No con razonamiento: con una medición que solo admite una respuesta. Con música
sonando, se leyó la sección de picos y se buscó un par de entradas que fueran una
el intercambio de bytes de la otra:

```
entrada 0x2040  ->  nivel 743     lleva la musica
entrada 0x4020  ->  nivel 0       silencio
```

Si el byte alto fuera el destino, ambas describirían caminos distintos sin
relación. Siendo `0x20 = MIXo:0` (salida 0 del mezclador) y `0x40 = InS0:0`
(salida física 1), solo una de las dos lecturas es compatible con que la música
salga por el altavoz: **`MIXo:0 → InS0:0`**. Byte alto, fuente.

La orientación corregida hace que todo el mapa encaje, y —prueba independiente—
coincide con el manual de Avid: el auricular A monitoriza la reproducción 1-2 y
el B la 3-4.

### El mapa real

| Bloque | Como fuente | Como destino |
|---|---|---|
| `InS0` | entradas analógicas 1-6 | salidas de línea 1-6 |
| `InS1` | — | auriculares |
| `AES` | entrada S/PDIF | salida S/PDIF |
| `ATX0` | **reproducción del Mac** | — |
| `ARX0` | — | **captura hacia el Mac** |
| `MIXo` | salidas del mezclador | — |
| `MIX0` | — | entradas del mezclador |

Entradas del mezclador: **0-5** ← entradas analógicas; **6-7** ← S/PDIF;
**8-15** ← reproducción del Mac (las tiras que el panel llama *Software Returns*).

Salidas del mezclador: **0/1** → salidas físicas 1-2 **y 3-4** (duplicadas);
**4/5** → salidas 5-6; **6/7** → S/PDIF; **8/9** → auricular A; **10/11** →
auricular B; **2/3 y 14/15** → `MUTED`, no van a ninguna parte.

### Detalle que muerde

La sección de picos **no lleva cabecera de cuenta**: la entrada 0 está en
`PEAK+0`. El router sí la lleva en su quadlet 0. Leer los picos con el desfase
del router desplaza toda la tabla una posición y produce un volcado que parece
coherente pero está mal.

---

## 2. Los tres fallos que impedían que la interfaz funcionara

El síntoma era "muevo los controles y no pasa nada". Eran tres causas
independientes apiladas.

### 2.1 El driver se ahogaba solo

Cada `SetMixerCoefficient` disparaba una **lectura completa de la tabla de
secciones del EAP** antes de escribir. La interfaz emite dos escrituras por cada
movimiento (izquierda y derecha), así que arrastrar un fader generaba cientos de
transacciones por segundo. La cola se saturaba y casi todas las escrituras se
perdían.

**Arreglo:** el desplazamiento del EAP se cachea una vez (no cambia tras la
enumeración) y cada celda admite **una escritura en vuelo con el último valor
fusionado detrás**. Un arrastre emite muchos más movimientos de los que el bus
puede llevar; lo que importa es que llegue el último, no todos.

**Y el agujero que eso abrió:** si la confirmación de una escritura se perdía —un
reinicio de bus se las traga— la celda quedaba marcada como ocupada para siempre
y ese fader dejaba de llegar al aparato durante el resto de la sesión. Síntoma:
*"desaparece el sonido y no vuelve"*. Se añadió un plazo: una escritura en vuelo
más de 500 ms se da por perdida y la celda se recupera.

### 2.2 Los medidores miraban el byte equivocado

El mismo error del router, propagado a la interfaz:

```swift
for (route, level) in peaks where (route >> 8) == destination   // mal
for (route, level) in peaks where (route & 0xFF) == destination // bien
```

Con el byte alto, la tira del canal 1 encontraba la entrada cuya **fuente** era
el bus de monitorización y mostraba la mezcla que salía por los altavoces. De ahí
el síntoma desconcertante de que *los canales 1 y 2 seguían el ritmo de los
canales 9 y 10, desfasados*: era la misma señal, una antes y otra después del
mezclador.

### 2.3 Se probaba una app distinta de la que se compilaba

`build.sh` escribe en `build/DerivedData`, pero la extensión registrada en
`sysextd` apuntaba al bundle de la carpeta de Xcode. Durante horas los arreglos
se compilaban en un sitio y se probaban en otro.

---

## 3. La cadena de despliegue, que era media batalla

### Compilar no es desplegar

`build.sh` compila con `CODE_SIGNING_ALLOWED=NO` **a propósito**: sirve para
comprobar que el código compila. El dext sale sin firmar y `sysextd` lo rechaza:

```
MacOS error: -67062
no policy, cannot allow apps outside /Applications
```

El `-67062` es `errSecCSUnsigned`, *"el objeto no está firmado en absoluto"*. El
mensaje sobre `/Applications` es una pista falsa: el problema es la firma. Con el
dext firmado ad-hoc, `sysextd` continúa sin problema desde la carpeta de Xcode.

Se creó **`deploy.sh`**: compila forzando la firma ad-hoc que el propio proyecto
define (`CODE_SIGN_IDENTITY: "-"` y los entitlements de DriverKit) y despliega en
la ruta registrada, en un paso.

### El número de versión

`bump.sh build` edita `project.yml`, pero `build.sh` regenera el proyecto **antes**
de llamar al bump, así que el número nuevo no se aplica hasta la compilación
siguiente. Un dext con la misma versión que el instalado no se sustituye, y el
Install parece no hacer nada.

### Las entradas atascadas de `sysextd`

Matar el dext con `pkill` mientras tiene el dispositivo de audio activo deja su
entrada en *"terminating for upgrade via delegate"* para siempre. Con dos
entradas, cualquier actualización posterior falla:

```
F  sysextd: activateDecision found two entries for none net.mrmidi.ASFW.ASFWDriver
```

Ni `systemextensionsctl uninstall` ni cerrar la app lo resuelven: el dext está
enganchado a la tarjeta por PCI y no sale. **Solo lo limpia un reinicio.**

**Regla:** no usar `pkill` sobre el dext. Si hay que forzar un relevo, reiniciar.

---

## 4. La matemática de Avid, recuperada y verificada

Extraída de `MixerUtils` en el binario del panel original y **confirmada contra
el hardware**.

### Paneo: potencia constante

```
centro = 0.7071  (-3 dB en ambos lados)
zona muerta alrededor del centro: 0.49990...0.50394
```

Verificación midiendo el nivel que llega al altavoz conectado:

| posición | nivel medido |
|---|---|
| todo izquierda | 1799 |
| centro | 1241 |
| todo derecha | 15 |

`20·log₁₀(1799/1241) = +3,2 dB`. La ley se comporta exactamente como predice.

### Fader

```
suelo   = -75 dB
codo    = -20 dB
reparto = 0.625 del recorrido por encima del codo
```

### Balance: NO es paneo

Un par enlazado ya trae una señal estéreo, así que su perilla es un **balance**:
ambos lados al máximo en el centro, y atenúa solo el lado del que te alejas.
Aplicar ahí la ley de potencia constante habría bajado el par entero 3 dB en el
centro.

```
izquierda = min(1, 2·(1-b))     derecha = min(1, 2·b)
```

---

## 5. Los medidores

**El aparato mide en un solo punto**: la señal que llega a la entrada del
mezclador, *antes* del fader. No hay un segundo medidor en hardware.

El panel de Avid ofrece las dos modalidades —en sus cadenas de texto están
`Pre-Fader Meters` y `Post-Fader Meters`, y los encabezados llevan el modo entre
paréntesis: `SOFTWARE RETURNS (Pre-Fader)`— así que el modo post-fader **se
calcula**, escalando la lectura con la ganancia del fader. Avid casi con
seguridad hace lo mismo, porque tampoco tiene de dónde sacarlo.

### La barra tenía que ser logarítmica

Era lineal en amplitud. Material que pica a −5 dBFS llena la mitad de la barra,
así que **el rojo del final no aparecía jamás** por mucho que se subieran los
faders. Ahora va en escala de decibelios, la misma que los números impresos junto
al fader (0, −6, −10, −20…), de modo que la lectura y la regla coinciden.

Que el fader esté en 0 **no** debe poner la barra en rojo: el tope del fader es
la unidad, deja pasar la señal sin amplificar. El rojo tiene que significar
"cerca de saturar". Un medidor que se pusiera rojo por la posición del fader y no
por la señal sería un medidor que miente.

### La perilla frontal no puede mover las barras

Escribe en seis niveles de salida de la sección *application*
(`EAP + 7001*4`, offsets `+0x28`..`+0x3c`), que se aplican **después** del
mezclador. El medidor está antes. Los faders deciden la mezcla; la perilla
decide a qué volumen se escucha. Se encadenan, no compiten.

---

## 6. El arranque del driver

`ApplyStartupRouter` tenía un cerrojo de una sola vez: al primer éxito marcaba
`startupRouterApplied_` y no volvía a mirar. Si una tanda de reinicios de bus
dejaba al aparato sin la tabla —cosa que ocurre, **y las escrituras siguen
informando de éxito**— ese cerrojo garantizaba que nadie la volviera a escribir.
El aparato se quedaba mudo hasta reprogramarlo a mano.

**Arreglo:** en vez de fiarse del cerrojo, lee del aparato cuántas entradas tiene
realmente y compara. Si no coinciden, reprograma. Tras escribir, vuelve a leer
para confirmar —un commit que informa de éxito no demuestra que la tabla esté
ahí— y reintenta una vez si no cuajó. Los coeficientes del mezclador cuelgan de
esa misma ruta, así que quedan cubiertos.

**Pendiente de validar en frío.** Además, que el router se programe dentro de la
puesta en marcha del audio es discutible: es configuración del aparato, no parte
del audio, y debería escribirse al enganchar el dispositivo.

---

## 7. Funciones de la original que ya existen aquí

**Link.** Debajo de cada par de tiras, como en la original (su arte sigue en el
kext: `ProTools.ButtonLink.*` y las barras `Grouper`). Une las dos tiras como un
solo canal estéreo: la izquierda va del todo a la izquierda, la derecha del todo
a la derecha, los faders se mueven juntos y la perilla pasa a ser balance.

Importa más de lo que parece: **dos tiras sin enlazar y centradas convierten una
señal estéreo en mono**, porque cada mitad se reparte por igual a las dos
salidas.

**Medidores pre/post-fader**, con el modo en los encabezados.

**Lectura de decibelios en vivo** sobre cada fader, con `-∞` en el fondo.

**Plaquitas H1-H8 / S1-S8**, porque los dos bancos numeraban `1..8` y eso hacía
ambigua cualquier conversación o captura de pantalla.

---

## 8. Herramientas de medición

En **`~/Developer/mbox-reference/tools/`**, fuera del repositorio y a salvo de
reinicios (el directorio de scratch se borra al arrancar y se perdió una vez):

| archivo | qué hace |
|---|---|
| `dev.py` | lee y escribe registros vía el servidor MCP de ASFW.app |
| `peaks.py` | vuelca la sección de picos con fuente y destino decodificados |
| `replay.py` | reprograma router y mezclador con la captura de Mojave |
| `watch.py` | vigila todas las celdas del mezclador mientras se mueven controles |

`asfw_write_quadlet` exige el campo **`value`**; con `payload` responde
`malformedRequest` y **la escritura no ocurre**. Una tanda entera de medidas se
dio por buena antes de descubrirlo.

---

## 9. Método: lo que funcionó y lo que no

Lo que hizo avanzar el trabajo fue, invariablemente, **leer el aparato en vez de
razonar sobre él**:

- El observador que vigilaba las celdas mientras el usuario movía un control
  resolvió en veinte segundos lo que tres rondas de parches a ciegas no habían
  aclarado: no llegaba ni una escritura.
- La orientación del router, invertida durante días, la zanjó un par de lecturas
  de picos con música sonando.
- La prueba de silenciar el mezclador y ver el nivel caer a `0` exacto, y volver,
  demostró la cadena entera sin ambigüedad.

Y lo que lo frenó fue lo contrario: **dar por buenas mediciones sin verificar**.
La prueba del mezclador se dio por concluyente cuando el script mandaba un campo
equivocado y no escribía nada. La conclusión que se sacó de ella era falsa, y
apuntaba en la dirección opuesta a la verdad.

---

## 10. Pendientes

### Interfaz

- [ ] **Etiquetar las pestañas de salida con su destino real** en vez de con
      números, y marcar la que va a `MUTED`. Las salidas físicas 3-4 son un
      duplicado de las 1-2, cosa que la interfaz no deja ver.
- [ ] **Medidor de máster** que lea la salida del mezclador (`MIXo:0/1`). Es el
      único sitio donde se puede ver saturar de verdad: la suma de varios canales
      sí puede pasarse de fondo de escala, y las barras actuales miden entradas.
- [ ] **Botón `Mono`** (está en las cadenas de la original). Resuelve además el
      caso de tener un solo altavoz conectado.
- [ ] **`Left Trim`** (también en las cadenas).
- [ ] Conectar el bloque de **efectos** y la **sección de máster**: los registros
      están mapeados pero no cableados.
- [ ] Los botones **S** y **M** son decorativos; no existe control de solo/mute
      por canal.
- [ ] Revisar las capturas de Mojave pendientes (`shot-2` … `shot-7`): Setup,
      Flow y los menús.
- [ ] **Sustituir el arte de Avid por diseños propios**, archivo a archivo.

### Driver

- [ ] **Validar en frío el arreglo del arranque**: que tras un arranque desde
      cero el aparato salga programado y sonando sin intervención.
- [ ] **Mover la programación del router al enganche del dispositivo**, no a la
      puesta en marcha del audio.
- [ ] Confirmar el **fondo de escala del medidor**. Se asume `4095` por
      observación (nunca se ha visto más), sin señal de referencia que lo
      confirme.
- [ ] Investigar los **timeouts del camino asíncrono**. Antes de un cuelgue
      aparecieron ~200 mensajes de *"el aparato confirma pero responde tarde"*.
      El audio isócrono siguió sano; se colgó el control. No está aclarado si lo
      provoca el tráfico de la interfaz o algo del driver.
- [ ] Llevar aguas arriba el arreglo del buzón de notificaciones y el perfil de
      la Mbox Pro.

### Preguntas abiertas

- [ ] Qué es el **bloque 3** como fuente.
- [ ] Por qué los canales **1 y 2** marcan nivel sin nada conectado.

### Operativa

- [ ] **`git commit`** de todo lo de hoy.
- [ ] Antes de instalar, mirar siempre `systemextensionsctl list`: si hay dos
      entradas, **reiniciar antes** de intentarlo.
