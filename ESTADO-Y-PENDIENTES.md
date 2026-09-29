# Estado y pendientes — 29 de septiembre de 2026

> **Cerrado el 29 de septiembre:** el soporte de la Mbox Pro está portado a la
> arquitectura actual de upstream (v0.3.1), verificado con el aparato sonando, y
> propuesto como pull request: **mrmidi/ASFireWire#162**.

Sustituye a la sección 10 de `MBOX_PRO_PANEL.md`, que queda como material del
artículo pero ya no como lista de trabajo.

---

## Qué funciona, medido

| Función | Estado |
|---|---|
| 6 salidas de línea | funciona |
| S/PDIF | enrutado |
| Auriculares A y B | funcionan |
| 4 entradas XLR | funcionan |
| **Todo simultáneo** | **sí** |
| Medidores del panel frontal | funcionan (muestran las entradas) |
| LED del panel | controlable por registro |
| Mezclador desde el host | escribe y responde, verificado |

Versión instalada y funcionando: **0.3.0 build 17**, sobre macOS 26.7 recién
instalado y compilada en esa misma máquina con Xcode 27.

**El ciclo completo está probado** (22 de septiembre): compilar → desplegar →
instalar → sonar. La build 15 de la partición vieja se rescató antes de borrarla
y sirvió de puente para arrancar sin compilar; la 17 se compiló ya en el sistema
nuevo. Compilación limpia desde cero: 45 segundos, cero errores, firma válida con
los cinco entitlements dentro.

La cadena de control se demostró de punta a punta: silenciar las dos tiras de
playback lleva el altavoz a **0 exacto** y restaurarlas lo devuelve. La ley de
paneo de potencia constante se midió en **+3,2 dB** de centro a extremo
(1799 / 1241 / 15).

---

## Decisión: el panel de control gráfico queda abandonado

No compensa. Los pendientes de interfaz están **cancelados**, no aplazados:
etiquetar pestañas, medidor de máster, botón `Mono`, `Left Trim`, bloque de
efectos, sección de máster, botones S/M, las capturas `shot-2`…`shot-7` y la
sustitución del arte de Avid por diseños propios.

Lo que **sí** se queda, porque es conocimiento del aparato y no de la interfaz:

- La orientación del router: **`(origen << 8) | destino`**. Byte alto origen,
  byte bajo destino. Estuvo documentado al revés durante días.
- La geometría del mezclador: 18 entradas × 16 salidas, índice
  `salida*18 + entrada`, unidad `0x4000`, coeficientes desde el offset +4.
- La sección de picos **no tiene cabecera de cuenta**: la entrada 0 está en
  `PEAK+0`, no en `PEAK+4`. Formato `(nivel << 16) | entrada_de_router`.
- El mapa pestaña → destino físico, y que las salidas **3-4 son un duplicado de
  las 1-2** porque ambas se alimentan de `MIXo:0/1`.
- La matemática de Avid (paneo, fader, balance), recuperada de `MixerUtils` y
  verificada contra el aparato. Está en `MBOX_PRO_PANEL.md`, sección 4.

---

## Limpieza antes de publicar — HECHA el 22 de septiembre de 2026

Se hizo, y con una variante deliberada: **nada se borró, todo se movió** a
`7-PANEL-ABANDONADO/`. El motivo apareció al comprobarlo: de los 9 archivos de
`ASFW/Resources/MboxPro/`, los 8 PNG sí tenían copia en `2-REFERENCIA-AVID/`,
pero **`panel-atlas.json` no** — y ese no es de Avid, es propio: el mapa de cómo
se recorta el atlas de sprites. Seguir el `rm -rf` al pie de la letra lo habría
perdido.

Resultado verificado: ninguna referencia al panel queda en el árbol, compila
limpio sin él (76 → 73 avisos) y el repositorio ya no necesita el arte de Avid
para construirse. **El panel nunca llegó a git**: se mantuvo fuera de los commits
desde el principio, así que la limpieza no produjo ningún cambio que commitear.

El procedimiento original queda abajo como registro de qué se tocó.

Cuatro pasos. Ninguno toca el driver.

**1. Borrar los tres archivos del panel:**

```bash
cd <repo>
rm ASFW/Views/MboxProControlView.swift
rm ASFW/ViewModels/MboxProControlViewModel.swift
rm ASFW/Views/Components/MboxPanelComponents.swift
```

**2. Desconectarlo de la navegación.** En `ASFW/Views/ModernContentView.swift`
hay exactamente cuatro referencias:

| Línea | Qué es |
|---|---|
| 59 | `case mboxPro = "Mbox Pro"` — la entrada del enum |
| 85 | el icono `"hifispeaker.and.homepod.fill"` |
| 145 | `case .mboxPro:` |
| 146 | `MboxProControlView(connector: debugVM.connector)` |

Quita las cuatro. No hay más usos en todo el árbol.

**3. Sacar el arte de Avid:**

```bash
rm -rf ASFW/Resources/MboxPro
```

Ya está en `.gitignore`, así que nunca llegó a git — pero sin el panel tampoco
tiene sentido que siga en el disco de trabajo. La copia de referencia se queda
en `2-REFERENCIA-AVID/`. **Deja la entrada del `.gitignore` puesta**: no estorba
y protege de un descuido futuro.

**4. Regenerar y compilar:**

```bash
xcodegen generate && ./build.sh
```

El target `ASFW` toma `sources: - path: ASFW`, la carpeta entera, así que borrar
archivos basta: no hay que tocar `project.yml`.

---

## Pendientes vivos

### Driver

- [x] **Validar en frío el arranque de la máquina** — hecho el 22 de septiembre,
      y sin querer: tras el reinicio que hizo falta para resolver el relevo de
      versiones, el dext arrancó solo desde el bundle nuevo y el aparato sonaba
      sin tocar nada. Reinicio de la máquina con la Mbox conectada y encendida:
      validado.
- [ ] **Falta validar el arranque en frío del aparato**, que es el otro caso:
      encender la Mbox **después** de que el Mac ya esté arrancado, y que salga
      programada y sonando sin intervención.
- [ ] **Sustituir un dext en caliente no funciona.** Reproducido en Tahoe limpio:
      la versión vieja se queda en `terminating for upgrade via delegate` y no
      suelta el controlador PCI, la nueva figura como `activated enabled` pero
      nunca arranca, y el aparato deja de sonar aunque siga apareciendo como
      dispositivo de audio. Solo se arregla reiniciando. Documentado como paso
      obligatorio en `5-INSTALAR/INSTALAR-EN-TAHOE.md`, pero **la causa no está
      investigada**: puede estar relacionado con los timeouts del camino asíncrono
      de aquí abajo.
- [ ] **Mover la programación del router al enganche del dispositivo**, no a la
      puesta en marcha del audio. Hoy el router lee 0 entradas hasta que arranca
      el audio, lo que confunde cualquier diagnóstico.
- [ ] **Confirmar el fondo de escala del medidor.** Se asume `4095` por
      observación — nunca se ha visto un valor mayor — pero sin señal de
      referencia que lo confirme.
- [ ] **Investigar los timeouts del camino asíncrono.** Tras instalar la v15
      aparecieron ~200 mensajes de *"el aparato confirma pero responde tarde"* y
      el control acabó colgado, mientras el audio isócrono seguía sano.
      Desenchufar y enchufar lo recuperó; reiniciar lo arregló del todo.
      **No está establecido si lo provocó el cambio de la v15.** Si vuelve a
      pasar, la forma de aislarlo es volver a la v14 y ver si desaparece.
- [ ] **Llevar aguas arriba** el arreglo del buzón de notificaciones (solo
      admitía un observador) y el perfil de la Mbox Pro.

### Preguntas abiertas

- [ ] Qué es el **bloque 3** como fuente. Es real y alimenta el auricular A, pero
      no se sabe qué produce.
- [ ] Por qué los canales **1 y 2** marcan nivel sin nada conectado.

### Operativa

- [x] **`git commit`** — hecho. **Siete commits locales** sobre `main`: el buzón
      de notificaciones, el arreglo de CLOCK_SELECT, el aparato entero (router,
      mezclador, picos), los tres documentos, el `deploy.sh` y el arreglo de su
      ruta de despliegue. El panel quedó fuera sin borrarse.
- [x] **Probar en el Tahoe nuevo** — hecho el 22 de septiembre. Suena.
- [ ] **Reautenticar GitHub.** El token del llavero se invalidó con la
      actualización a Tahoe: `gh auth login -h github.com`. Bloquea todo lo de
      abajo.
- [ ] **Publicar el fork** en `github.com/KinoLab07/ASFireWire` y empujar `main`
      con los siete commits.
- [x] **PR abierto: mrmidi/ASFireWire#162**, desde la rama `avid-port`. 12
      archivos, 374 líneas. No es la rama `avid-mbox-pro` que se preparó el 22:
      upstream avanzó 370 commits en una semana y retiró la arquitectura entera
      sobre la que se apoyaba (`DeviceProtocolFactory`, `AudioProfileRegistry`,
      `DICEDuplexBringupController`). Hubo que re-expresarlo, no rebasarlo.
- [x] **Los dos arreglos de DICE quedaron obsoletos**, y por buenas razones. El
      del buzón lo resolvió upstream mejor (`fabd595e`: uno por dispositivo más
      un router que atribuye cada escritura, lo que además arregla que el
      CLOCK_ACCEPTED de un aparato terminase la espera de otro). Y el de
      CLOCK_SELECT lo encontró mrmidi por su cuenta tres días después
      (`b1d80f39`), validándolo con una Saffire Pro 24 DSP, una Venice F24 y una
      MultiMix. Dos personas, dos aparatos distintos, el mismo diagnóstico.
- [ ] **Borrar la rama `respaldo-antes-de-reescribir`** una vez empujado. Existe
      porque se reescribió el mensaje de un commit que contenía una afirmación
      falsa sobre el `Info.plist` (ver abajo).
- [ ] Antes de instalar, **siempre** `systemextensionsctl list`. Dos entradas →
      reiniciar antes de intentarlo.

---

## Lo que ya se decidió y no hay que volver a investigar

- **Ejecutar el Panel original de Avid en Apple Silicon es imposible.** Tres
  bloqueos independientes y verificados: la CPU es arm64 y Rosetta traduce
  x86_64, no i386; macOS retiró los 32 bits en Catalina; `libstdc++.6.dylib` ya
  no existe y Carbon, AGL y OpenGL son cascarones. La única vía real sería un Mac
  Intel con Mojave.
- **El aparato no está limitado a un banco de salida a la vez.** Durante dos días
  se concluyó eso y se formuló una regla —"gana el puerto con más referencias
  como fuente"— que encajaba con siete experimentos y era falsa. Describía el
  defecto de nuestras propias tablas, que saltaban el mezclador.
- **Sin coeficientes de mezclador no suena nada**, porque las salidas se
  alimentan de `MIXo`. La tabla de rutas sola es media configuración.
- **No existe ningún diálogo de "permitir igualmente"** para los entitlements
  restringidos: los valida AMFI contra un perfil firmado por Apple, y el "Allow"
  de Ajustes del Sistema aprueba *cargar* una extensión ya firmada válidamente,
  que es otra puerta. Esto es firme.
- **Pero "instalar con la seguridad íntegra no es posible" era demasiado fuerte.**
  Corregido el 21 de septiembre: Apple introdujo variantes **"development"** de
  los entitlements de DriverKit, que no requieren aprobación y permiten dejar SIP
  **activado**. El muro real es la cuota de 99 $/año — con Apple ID gratuito no
  hay perfil de DriverKit. Lo que sigue necesitando concesión de Apple, Developer
  ID y notarización es **distribuir a terceros**, no el uso propio. Detalle
  completo y las dos incógnitas que lo hundirían en
  `5-INSTALAR/INSTALAR-EN-TAHOE.md`, apartado SIP.

---

## La lección de método

Lo que hizo avanzar el trabajo fue, invariablemente, **leer el aparato en vez de
razonar sobre él**. El observador que vigilaba las celdas del mezclador resolvió
en veinte segundos lo que tres rondas de parches a ciegas no habían aclarado: no
llegaba ni una escritura.

Y lo que lo frenó fue **dar por buenas mediciones sin verificar el instrumento**.
Una prueba entera del mezclador se dio por concluyente cuando el script mandaba
el campo equivocado y no escribía nada; la conclusión que se sacó de ella era
falsa y apuntaba en dirección contraria a la verdad.
