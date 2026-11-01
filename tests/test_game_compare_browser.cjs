// Synthetic fixtures only; nothing is added to the public catalog.
// Run: node tests/test_game_compare_browser.cjs
// Uses local playwright or NODE_PATH. Never installs dependencies or browsers.
// Optional PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH selects an existing executable.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const {pathToFileURL} = require('node:url');
const {execFileSync} = require('node:child_process');
const root = path.resolve(__dirname, '..');

async function main() {
  let playwright;
  try { playwright = require('playwright'); } catch (error) {
    if (error.code !== 'MODULE_NOT_FOUND') throw error;
    playwright = require(path.join(root,'.local/game-compare-browser/node_modules/playwright'));
  }
  const template = await fs.readFile(path.join(root,'docs/compare/index.html'),'utf8');
  const marker = '<script id="catalog-data" type="application/json">';
  assert(template.includes('<body>'));
  assert(template.includes(marker));
  const catalog = JSON.parse(template.split(marker)[1].split('</script>')[0]);
  assert.equal(catalog.schema,1);
  assert(Array.isArray(catalog.scenes),'Public catalog scenes must be an array');
  assert(catalog.scenes.every(scene=>scene !== null && typeof scene === 'object' && scene.status === 'approved'),'Public catalog must contain only approved scenes');
  await fs.mkdir(path.join(root,'tmp'),{recursive:true});
  const directory = await fs.mkdtemp(path.join(root,'tmp/compare-browser-'));
  let browser;
  try {
    const launch = {headless:true,args:['--force-color-profile=srgb','--force-hdr=srgb']};
    if (process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH) launch.executablePath = process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH;
    browser = await playwright.chromium.launch(launch);
    const context = await browser.newContext({viewport:{width:1280,height:900},hasTouch:true});
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', error=>errors.push(error.message));
    // Ordinary SDR JPEGs stand in for Ultra HDR sources: test routing and layout, not HDR rendering.
    for (const [name,color] of [['off-hdr','#cc3344'],['on-hdr','#2277dd']]) {
      await page.setContent('<body style="margin:0;background:'+color+'"></body>');
      await page.screenshot({path:path.join(directory,name+'.jpg'),type:'jpeg',clip:{x:0,y:0,width:800,height:450}});
    }
    for (const [name,color,width,height] of [['off','#cc3344',800,450],['on','#2277dd',800,450],['wrong','#ffffff',400,450],['wide-off','#cc3344',2100,900],['wide-on','#2277dd',2100,900]]) {
      const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="'+width+'" height="'+height+'"><rect width="100%" height="100%" fill="'+color+'"/></svg>';
      await fs.writeFile(path.join(directory,name+'.svg'),svg);
    }
    const base = {id:'game/scene/hash',game:'Game A',scene:'Scene A',off:'off.svg',on:'on.svg',preview:'off.svg',width:800,height:450,status:'approved',labels:{off:'DLSS 5 OFF',on:'DLSS 5 ON'},metadata:{api:'DX12',gpu:'Synthetic GPU'},warnings:[],synthetic:false};
    let serial = 0;
    async function open(scenes,{candidate=false,hash='',raw,hdrEnvironment}={}) {
      const data = raw === undefined ? JSON.stringify({schema:1,scenes}).replaceAll('<',String.fromCharCode(92)+'u003c') : raw;
      const start = template.indexOf(marker)+marker.length;
      const end = template.indexOf('</script>',start);
      let html = template.slice(0,start)+data+template.slice(end);
      if (hdrEnvironment !== undefined) {
        // Exercise both hint branches independently of this headless machine's display capability.
        html = html.replace('<head>','<head><script>const originalMatchMedia=window.matchMedia.bind(window);window.matchMedia=query=>query==="(dynamic-range: high)"?{matches:'+JSON.stringify(hdrEnvironment)+',addEventListener(){}}:originalMatchMedia(query);</script>');
      }
      if (candidate) html = html.replace('<body>','<body data-candidate="true">');
      const file = path.join(directory,String(++serial)+'.html');
      await fs.writeFile(file,html);
      await page.goto(pathToFileURL(file).href+hash);
    }
    async function ready() { await page.waitForFunction(()=>!document.getElementById('split').disabled); }
    async function split(value) { assert.equal(await page.locator('#split').inputValue(),String(value)); }
    async function assertGeometry(scene,label) {
      const geometry = await page.locator('#stage').evaluate(stage=>{
        const s = stage.getBoundingClientRect();
        return {width:s.width,height:s.height,layers:[...stage.querySelectorAll('.layer')].map(node=>{
          const r = node.getBoundingClientRect();
          return {x:r.x-s.x,y:r.y-s.y,width:r.width,height:r.height,naturalWidth:node.naturalWidth,naturalHeight:node.naturalHeight};
        })};
      });
      // Chromium layout rounds to fractional CSS pixels; allow less than 1/32 px.
      const close = (actual,expected,description)=>assert(Math.abs(actual-expected)<.02,label+': '+description+' ('+actual+' vs '+expected+')');
      assert(geometry.width>0 && geometry.height>0,label+': visible image area');
      close(geometry.height,geometry.width*scene.height/scene.width,'stage preserves source aspect ratio');
      assert.equal(geometry.layers.length,2);
      assert.deepEqual(geometry.layers[0],geometry.layers[1],label+': both layers share coordinates and dimensions');
      for (const layer of geometry.layers) {
        assert.equal(layer.naturalWidth,scene.width);
        assert.equal(layer.naturalHeight,scene.height);
        close(layer.x,0,'layer left'); close(layer.y,0,'layer top');
        close(layer.width,geometry.width,'layer fills stage width');
        close(layer.height,geometry.height,'layer fills stage height');
        close(layer.height,layer.width*scene.height/scene.width,'layer preserves source aspect ratio');
      }
      return geometry;
    }
    async function continuousTouch(scene,label) {
      await page.locator('#stage').scrollIntoViewIfNeeded();
      const rectangle = await page.locator('#stage').boundingBox();
      const before = await assertGeometry(scene,label);
      const y = rectangle.y+rectangle.height*.5;
      assert(y>0 && y<(await page.viewportSize()).height,label+': touch lies inside viewport');
      await page.locator('#stage').evaluate(stage=>{
        globalThis.touchEvidence = [];
        for (const type of ['pointerdown','pointermove','pointerup','pointercancel']) stage.addEventListener(type,event=>{
          if (event.pointerType==='touch') globalThis.touchEvidence.push(event.type);
        });
      });
      const cdp = await context.newCDPSession(page);
      const point = percent=>({id:1,x:rectangle.x+rectangle.width*percent/100,y,radiusX:1,radiusY:1,force:1});
      try {
        await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[point(15)]});
        await split(15);
        // Assert every intermediate update, including reversal, before ending the gesture.
        for (const percent of [25,45,65,85,35]) {
          await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[point(percent)]});
          await page.waitForFunction(expected=>Number(document.getElementById('split').value)===expected,percent);
          assert.deepEqual(await assertGeometry(scene,label),before,label+': continuous touch keeps image geometry fixed');
        }
        await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
        await split(35);
        const events = await page.evaluate(()=>globalThis.touchEvidence);
        assert.equal(events[0],'pointerdown',label+': actual touch pointer started');
        assert.equal(events.at(-1),'pointerup',label+': actual touch pointer ended');
        assert(events.filter(type=>type==='pointermove').length>=5,label+': actual continuous touch movement');
        assert(!events.includes('pointercancel'),label+': touch must not become viewport scrolling');
      } finally { await cdp.detach(); }
      console.log('PASS: '+label+'; layers '+before.width+' x '+before.height+' CSS px; CDP touch 15→25→45→65→85→35, end=35');
    }
    await open([]);
    assert.equal(await page.locator('html').getAttribute('lang'),'zh-CN');
    assert.equal(await page.locator('#gallery').isVisible(),false);
    assert(await page.locator('#empty').isVisible());
    await page.locator('#language').click();
    assert.equal(await page.locator('h1').textContent(),'Image comparison');
    await open([{...base,status:'candidate'}]);
    assert.equal(await page.locator('#gallery').isVisible(),false);
    const second = {...base,id:'other/scene/2',game:'Game B',scene:'Scene B'};
    const unsafe = ['../off.svg','/off.svg','https://invalid.example/off.svg','data:image/png,x','%2e%2e/off.svg','folder/../off.svg','a'+String.fromCharCode(92)+'off.svg','off.svg?x','off.svg#x'];
    await open([base,second,{...base,id:'hidden',status:'candidate'},...unsafe.map((off,i)=>({...base,id:'unsafe'+i,off}))],{hash:'#scene='+encodeURIComponent(second.id)});
    await ready();
    assert.equal(await page.locator('#game').inputValue(),'Game B');
    assert.equal(await page.locator('#game option').count(),2);
    await page.locator('#game').selectOption('Game A'); await ready();
    assert.equal(await page.locator('#scene option').count(),1);
    assert.equal(await page.evaluate(()=>decodeURIComponent(location.hash)),'#scene='+base.id);
    await page.goBack(); await ready();
    assert.equal(await page.locator('#scene').inputValue(),second.id,'History back restores the original deep link');
    await page.goForward(); await ready();
    assert.equal(await page.locator('#scene').inputValue(),base.id,'History forward restores scene selection');
    const bounds = ()=>page.locator('.layer').evaluateAll(nodes=>nodes.map(node=>{const r=node.getBoundingClientRect(),s=node.parentElement.getBoundingClientRect();return [r.x-s.x,r.y-s.y,r.width,r.height];}));
    const before = await bounds(); assert.deepEqual(before[0],before[1]);
    await page.locator('#all-on').click(); await split(0);
    assert.equal(await page.locator('#on-image').evaluate(node=>getComputedStyle(node).clipPath),'inset(0px 0px 0px 0%)');
    await page.locator('#all-off').click(); await split(100);
    assert.equal(await page.locator('#on-image').evaluate(node=>getComputedStyle(node).clipPath),'inset(0px 0px 0px 100%)');
    await page.locator('#split').focus();
    await page.keyboard.press('Home'); await split(0);
    await page.keyboard.press('ArrowRight'); await split(1);
    await page.keyboard.press('End'); await split(100);
    await page.keyboard.press('ArrowLeft'); await split(99);
    const stage = await page.locator('#stage').boundingBox();
    await page.mouse.move(stage.x+stage.width*.2,stage.y+stage.height*.5);
    await page.mouse.down(); await page.mouse.move(stage.x+stage.width*.7,stage.y+stage.height*.5); await page.mouse.up();
    assert(Math.abs(Number(await page.locator('#split').inputValue())-70)<=1);
    await page.touchscreen.tap(stage.x+stage.width*.25,stage.y+stage.height*.5);
    assert(Math.abs(Number(await page.locator('#split').inputValue())-25)<=1);
    assert.deepEqual(await bounds(),before,'Dragging must not move or resize either layer');
    await page.locator('#stage').dblclick(); await split(50);
    await page.locator('#all-off').click(); await page.locator('#reset').click(); await split(50);
    assert.equal(await page.locator('#open-off').getAttribute('href'),'off.svg');
    assert.equal(await page.locator('#open-on').getAttribute('href'),'on.svg');
    assert.equal(await page.locator('#hdr-controls').isVisible(),false);
    assert.equal(await page.locator('#open-hdr-preview').isVisible(),false);
    await page.locator('#expand').click();
    assert(await page.locator('#viewer').evaluate(node=>node.classList.contains('expanded')));
    await page.keyboard.press('Escape');
    assert.equal(await page.locator('#viewer').evaluate(node=>node.classList.contains('expanded')),false);
    if (await page.locator('#fullscreen').isVisible()) {
      await page.locator('#fullscreen').click();
      await page.waitForFunction(()=>document.fullscreenElement?.id==='viewer');
      await page.locator('#fullscreen').click();
      await page.waitForFunction(()=>!document.fullscreenElement);
    }
    const wide = {...base,id:'wide/scene/21-9',game:'Ultrawide synthetic',scene:'21:9 synthetic',off:'wide-off.svg',on:'wide-on.svg',width:2100,height:900,synthetic:true};
    for (const scene of [base,wide]) {
      await open([scene]); await ready();
      for (const viewport of [{width:1280,height:900},{width:390,height:844}]) {
        await page.setViewportSize(viewport);
        const label = scene.width+':'+scene.height+' at '+viewport.width+'x'+viewport.height;
        assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),label+': no horizontal overflow');
        await continuousTouch(scene,label);
      }
    }
    await page.setViewportSize({width:1280,height:900});
    const hdr = {...base,id:'hdr/scene/pair',scene:'Synthetic HDR routing',dynamic_range:'ultra-hdr',off:'off-hdr.jpg',on:'on-hdr.jpg',sdr_off:'off.svg',sdr_on:'on.svg',hdr_preview:'on-hdr.jpg',synthetic:true};
    const unsafeHdr = ['sdr_off','sdr_on','hdr_preview','preview'].flatMap(key=>unsafe.map((value,i)=>({...hdr,id:'unsafe-'+key+'-'+i,[key]:value})));
    const missingHdrSdr = {...hdr,id:'missing-sdr'}; delete missingHdrSdr.sdr_on;
    await open([hdr,...unsafeHdr,missingHdrSdr],{hdrEnvironment:false}); await ready();
    assert.equal(await page.locator('#scene option').count(),1,'Every new path field must reject unsafe relative paths');
    assert(await page.locator('#hdr-controls').isVisible());
    assert.equal(await page.locator('#display-mode').inputValue(),'hdr');
    assert((await page.locator('#hdr-help').textContent()).includes('否则显示 SDR 底图'));
    assert((await page.locator('#hdr-environment').textContent()).includes('未报告'));
    assert((await page.locator('#hdr-environment').textContent()).includes('不确认'));
    assert((await page.locator('#metadata').textContent()).includes('Ultra HDR JPEG + SDR 备用图'));
    assert.equal(await page.locator('#open-hdr-preview').getAttribute('href'),hdr.hdr_preview);
    await page.locator('#language').click();
    assert.equal(await page.locator('label[for=display-mode]').textContent(),'Display mode');
    assert((await page.locator('#hdr-help').textContent()).includes('compatible browser, operating system and display'));
    assert((await page.locator('#hdr-help').textContent()).includes('otherwise the SDR base image'));
    assert((await page.locator('#hdr-environment').textContent()).includes('does not confirm'));
    for (const viewport of [{width:1280,height:900},{width:390,height:844}]) {
      await page.setViewportSize(viewport);
      for (const mode of ['hdr','sdr']) {
        await page.locator('#display-mode').selectOption(mode); await ready();
        const prefix = mode === 'hdr' ? '' : 'sdr_';
        for (const side of ['off','on']) {
          const expected = hdr[prefix+side];
          assert.equal(await page.locator('#open-'+side).getAttribute('href'),expected);
          assert((await page.locator('#'+side+'-image').getAttribute('src')).endsWith('/'+expected));
        }
        assert.equal(await page.locator('#open-hdr-preview').getAttribute('href'),hdr.hdr_preview);
        const geometry = await assertGeometry(hdr,mode+' at '+viewport.width);
        await page.locator('#split').evaluate(node=>{node.value='37';node.dispatchEvent(new Event('input',{bubbles:true}));});
        await page.locator('#display-mode').selectOption(mode === 'hdr' ? 'sdr' : 'hdr'); await ready();
        await split(37);
        assert.deepEqual(await assertGeometry(hdr,'mode switch'),geometry,'HDR/SDR mode switch preserves image geometry');
        assert.equal(await page.locator('#on-image').evaluate(node=>getComputedStyle(node).clipPath),'inset(0px 0px 0px 37%)');
        assert(await page.locator('.layer').evaluateAll(nodes=>nodes.every(node=>node.tagName==='IMG' && getComputedStyle(node).filter==='none' && getComputedStyle(node).mixBlendMode==='normal')));
        assert.equal(await page.locator('canvas').count(),0);
        assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'HDR controls do not overflow mobile width');
      }
    }
    await page.setViewportSize({width:1280,height:900});
    // Competing decode promises must leave the last selected source and links in place.
    await page.locator('#display-mode').evaluate(node=>{
      for (const mode of ['sdr','hdr']) {node.value=mode;node.dispatchEvent(new Event('change',{bubbles:true}));}
    }); await ready();
    assert((await page.locator('#off-image').getAttribute('src')).endsWith('/off-hdr.jpg'));
    assert.equal(await page.locator('#open-off').getAttribute('href'),'off-hdr.jpg');
    await open([hdr,base],{hdrEnvironment:true}); await ready();
    assert((await page.locator('#hdr-environment').textContent()).includes('报告支持高动态范围'));
    assert((await page.locator('#hdr-environment').textContent()).includes('不确认'));
    await page.locator('#language').click();
    assert((await page.locator('#hdr-environment').textContent()).includes('reports high dynamic range support'));
    assert((await page.locator('#hdr-environment').textContent()).includes('does not confirm'));
    await page.locator('#display-mode').selectOption('sdr'); await ready();
    assert((await page.locator('#metadata').textContent()).includes('Current image sourceSDR'));
    await page.locator('#scene').selectOption(base.id); await ready();
    assert.equal(await page.locator('#hdr-controls').isVisible(),false);
    assert.equal(await page.locator('#open-hdr-preview').getAttribute('href'),null);
    assert.equal(await page.locator('#open-off').getAttribute('href'),base.off);
    await page.locator('#scene').selectOption(hdr.id); await ready();
    assert.equal(await page.locator('#display-mode').inputValue(),'hdr','Entering an HDR scene defaults to Ultra HDR');
    const noPreview = {...hdr,status:'candidate'}; delete noPreview.hdr_preview;
    await open([noPreview],{candidate:true}); await ready();
    assert.equal(await page.locator('#open-hdr-preview').isVisible(),false);
    assert((await page.locator('#review-banner').textContent()).includes('待审核'));
    await page.locator('#display-mode').selectOption('sdr'); await ready();
    await page.locator('#language').click();
    assert((await page.locator('#review-banner').textContent()).includes('Pending review'));
    await open([{...hdr,sdr_on:'wrong.svg'}]); await ready();
    await page.locator('#display-mode').selectOption('sdr');
    await page.waitForFunction(()=>document.getElementById('stage').getAttribute('aria-busy')==='false');
    assert(await page.locator('#split').isDisabled(),'Invalid SDR dimensions must not reveal a mismatched pair');
    await page.locator('#display-mode').selectOption('hdr'); await ready();
    const embedded = {...hdr,game:'中文游戏',scene:'中文场景',game_en:'English Game',scene_en:'English Scene'};
    delete embedded.sdr_off; delete embedded.sdr_on; delete embedded.hdr_preview;
    await open([embedded,base]); await ready();
    assert.equal(await page.locator('#display-mode').isVisible(),false,'No separate SDR mode is offered without fallback assets');
    assert((await page.locator('#hdr-help').textContent()).includes('内置 SDR 底图'));
    await page.locator('#language').click();
    assert.equal(await page.locator('#game option:checked').textContent(),'English Game');
    assert.equal(await page.locator('#scene option:checked').textContent(),'English Scene');
    assert((await page.locator('#off-image').getAttribute('alt')).includes('English Game / English Scene'));
    assert((await page.locator('#hdr-help').textContent()).includes('without a separate SDR file'));
    await page.locator('#game').selectOption(base.game); await ready();
    assert.equal(await page.locator('#scene option:checked').textContent(),base.scene,'Legacy labels fall back without English fields');
    // Compare real Ultra HDR against its extracted primary JPEG under forced SDR Chromium.
    const python = process.env.GAME_COMPARE_PYTHON || path.join(root,'.local/game-compare-venv/Scripts/python.exe');
    const fixtureScript = [
      'import sys; from pathlib import Path; import numpy as np; import imagecodecs as c',
      'sys.path.insert(0,str(Path(sys.argv[1])/"tools")); import game_compare_hdr as h',
      'rgb=np.ones((144,256,4),np.float32); rgb[...,:3]=np.linspace(.05,10,256)[None,:,None]',
      'intent,_=h.hdr_intent(rgb)',
      'data=h.encode(intent,h.tonemap_sdr(rgb),dict(quality=95,gainmap_scale=2,gainmap_quality=95,gainmap_channels=3,gainmap_gamma=1,shared_target_peak_nits=800))',
      'out=Path(sys.argv[2]); (out/"embedded.jpg").write_bytes(data); (out/"base.jpg").write_bytes(c.ultrahdr_decode_jpeg(data)[0])'
    ].join('\n');
    execFileSync(python,['-B','-c',fixtureScript,root,directory],{stdio:'pipe'});
    await page.setViewportSize({width:1280,height:900});
    await page.setContent('<body style="margin:0"><img id="embedded" src="'+pathToFileURL(path.join(directory,'embedded.jpg')).href+'"><img id="base" src="'+pathToFileURL(path.join(directory,'base.jpg')).href+'"></body>');
    await page.locator('img').evaluateAll(nodes=>Promise.all(nodes.map(node=>node.decode())));
    assert.deepEqual(await page.locator('#embedded').screenshot(),await page.locator('#base').screenshot(),'Ultra HDR automatically falls back to the same primary JPEG in forced SDR');
    console.log('PASS: real synthetic gain-map JPEG vs extracted base, forced SDR browser pixel equality; no physical HDR validation');
    console.log('PASS: synthetic JPEG/SVG HDR routing, default source, bilingual conditional hints, HDR/SDR mode and full-image links, safe paths, race recovery, fixed desktop/mobile clip geometry; no physical HDR validation');
    const hostile = '<img src=x onerror="globalThis.injected=true">';
    await open([{...base,game:hostile,scene:hostile,metadata:{gpu:hostile},warnings:[hostile]}]); await ready();
    assert.equal(await page.locator('#game option').textContent(),hostile);
    assert.equal(await page.locator('#metadata img, #warnings img').count(),0);
    assert.equal(await page.evaluate(()=>globalThis.injected),undefined);
    await open([{...base,status:'candidate',synthetic:true}],{candidate:true}); await ready();
    assert(await page.locator('#review-banner').isVisible());
    assert((await page.locator('#review-banner').textContent()).includes('合成测试'));
    await page.locator('#language').click();
    assert((await page.locator('#review-banner').textContent()).includes('Synthetic test'));
    assert((await page.locator('#review-banner').textContent()).includes('Pending review'));
    assert((await page.locator('#open-on').textContent()).includes('processed'));
    await open([{...base,on:'wrong.svg'}]);
    await page.waitForFunction(()=>document.getElementById('stage').getAttribute('aria-busy')==='false');
    assert(await page.locator('#split').isDisabled());
    assert(await page.locator('#stage').evaluate(node=>node.classList.contains('loading')));
    await open([],{raw:'{broken'});
    assert(await page.locator('#empty').isVisible());
    assert.equal(await page.locator('#gallery').isVisible(),false);
    assert.deepEqual(errors,[]);
    console.log('Browser: '+browser.version());
    console.log('PASS: 4 geometry cases and 4 continuous CDP touch gestures (5 checked moves each), 16:9 and 21:9 at desktop and 390x844');
    console.log('PASS: file://, catalog filtering, bilingual UI, deep links, keyboard/mouse/touch, image alignment, mobile layout, candidate/synthetic banners, XSS, image failures');
  } finally {
    try { if (browser) await browser.close(); } finally { await fs.rm(directory,{recursive:true,force:true}); }
  }
}
main().catch(error=>{console.error(error.stack);process.exitCode=1;});
