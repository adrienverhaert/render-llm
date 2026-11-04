import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn,spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {StdioClientTransport} from '@modelcontextprotocol/sdk/client/stdio.js';
const arg=k=>process.argv[process.argv.indexOf(k)+1];
const exe=path.resolve(arg('--exe')), scenePath=path.resolve(arg('--scene')), evidence=path.resolve(arg('--evidence'));
const runtime=path.dirname(exe),dir=path.join(runtime,'Data/assistant');
const groupTransform=process.argv.includes('--group-transform');
const editor=spawn(exe,[],{cwd:runtime,windowsHide:true,stdio:'ignore',env:{...process.env,RENDER_LLM_TEST_MENU:'1'}});
const client=new Client({name:'studio-native-check',version:'1'});
let httpHost;
const get=r=>{if(r.structuredContent?.success!==true)throw new Error(JSON.stringify(r));return r.structuredContent;};
const call=async(name,args={})=>get(await client.callTool({name,arguments:args},undefined,{timeout:180000}));
const pixels=file=>{
 const decoded=spawnSync('ffmpeg',['-hide_banner','-v','error','-i',file,'-f','rawvideo','-pix_fmt','rgb24','pipe:1'],{windowsHide:true,maxBuffer:2*1024*1024});
 if(decoded.error||decoded.status!==0||decoded.stdout.length!==384*216*3)throw new Error(`Image decode failed: ${file}: ${decoded.error??decoded.stderr.toString()}`);
 return createHash('sha256').update(decoded.stdout).digest('hex');
};
fs.mkdirSync(evidence,{recursive:true});
try{
 await new Promise(r=>setTimeout(r,3500));
 await client.connect(new StdioClientTransport({command:process.execPath,args:[path.join(runtime,'ASSISTANT/dist/mcp-stdio.js'),'--dir',dir],stderr:'pipe'}));
 const menuStart=Date.now();const initial=await call('scene_get');const menuMs=Date.now()-menuStart;
 if(!/STUDIO_MENU_OPEN=:\s*1/.test(fs.readFileSync(path.join(runtime,'Data/log.txt'),'utf8')))throw new Error('The real dropdown fixture did not open');
 const seedDoc=JSON.parse(fs.readFileSync(scenePath,'utf8'));
 if(process.argv.includes('--renderer'))seedDoc.render={...seedDoc.render,schema:2,renderer:arg('--renderer')};
 if(process.argv.includes('--legacy-renderer')){seedDoc.render.renderer='Flame';delete seedDoc.render.schema;}
 seedDoc.scene.world={...seedDoc.scene.world,ambient_color:'#112233',sun_color:'#FFF0DD',elevation:-32,rotation:17,angular_size:2,fog_enabled:false,fog_color:'#CCDDEE',fog_distance:8000,fog_height:900};
 seedDoc.render={...seedDoc.render,bounces:4,noise_threshold:.03,denoise:true,exposure:1.25,overscan:0,tiled:true};
 if(groupTransform){
  if(seedDoc.scene.objects.some(x=>x.id==='studio-test-group'))throw new Error('Group test ID collision');
  for(const object of seedDoc.scene.objects.filter(x=>x.kind==='sphere'||x.kind==='cylinder')){
   if(object.parent_id)throw new Error('Group test requires unparented shapes');
   object.parent_id='studio-test-group';
  }
  seedDoc.scene.objects.push({id:'studio-test-group',name:'Studio assembly',kind:'group',position:[0,0,0],rotation:[0,0,0],scale:[1,1,1],visible:true});
 }
 const seed=JSON.stringify(seedDoc);await call('scene_validate',{document:seed});await call('scene_apply',{document:seed,expected_revision:initial.revision});
 const baseline=await call('scene_get'),doc=JSON.parse(baseline.document);
 if(groupTransform){
  const group=doc.scene.objects.find(x=>x.id==='studio-test-group');
  if(group?.kind!=='group'||doc.scene.objects.filter(x=>x.parent_id===group.id).length!==2)throw new Error('Native group hierarchy was not preserved');
  fs.writeFileSync(path.join(evidence,'group-baseline.renderllm'),baseline.document);
 }
 if(process.argv.includes('--renderer')&&doc.render.renderer!==arg('--renderer'))throw new Error('Renderer selection was not preserved');
 if(process.argv.includes('--legacy-renderer')&&doc.render.renderer!=='UltraFlame')throw new Error('Legacy Flame file changed its engine');
 for(const [key,value] of Object.entries(seedDoc.scene.world))if(JSON.stringify(doc.scene.world[key])!==JSON.stringify(value))throw new Error(`World field lost: ${key}`);
 for(const key of ['bounces','noise_threshold','denoise','exposure','overscan','tiled'])if(doc.render[key]!==seedDoc.render[key])throw new Error(`Render field lost: ${key}`);
 const sphere=doc.scene.objects.find(x=>x.kind==='sphere'),pedestal=doc.scene.objects.find(x=>x.kind==='cylinder');
 if(!sphere||!pedestal||Math.abs(sphere.position[2]-pedestal.position[2]-16*pedestal.scale[2])>.1)throw new Error('Invalid contact');
 const render=await call('render_image',{expected_revision:baseline.revision,width:384,height:216,samples:16,denoise:true});
 fs.copyFileSync(path.join(dir,render.image),path.join(evidence,'studio.png'));
 if(groupTransform)doc.scene.objects.find(x=>x.id==='studio-test-group').position[0]=8;
 else doc.materials[sphere.material_id].color='#00FFFF';
 await call('scene_validate',{document:JSON.stringify(doc)});
 const edited=await call('scene_apply',{document:JSON.stringify(doc),expected_revision:baseline.revision});
 const observed=await call('scene_get');const observedDoc=JSON.parse(observed.document);
 if(groupTransform){
  if(observedDoc.scene.objects.find(x=>x.id==='studio-test-group')?.position[0]!==8)throw new Error('Group edit not observed');
  if(JSON.stringify(observedDoc.materials)!==JSON.stringify(doc.materials))throw new Error('Group edit changed materials');
  fs.writeFileSync(path.join(evidence,'group-edited.renderllm'),observed.document);
 }else if(observedDoc.materials[sphere.material_id].color!=='#00FFFF')throw new Error('Edit not observed');
 let pixelCycle;
 if(process.argv.includes('--render-edits')){
  const image=await call('render_image',{expected_revision:observed.revision,width:384,height:216,samples:16,denoise:true});
  fs.copyFileSync(path.join(dir,image.image),path.join(evidence,'edited.png'));
  pixelCycle={baseline_rgb_sha256:pixels(path.join(evidence,'studio.png')),edited_rgb_sha256:pixels(path.join(evidence,'edited.png')),edited_render:image};
  if(pixelCycle.baseline_rgb_sha256===pixelCycle.edited_rgb_sha256)throw new Error('The scene edit did not change rendered pixels');
 }
 await call('undo',{expected_revision:edited.revision});const restored=await call('scene_get');
 if(restored.document!==baseline.document)throw new Error('Undo mismatch');
 if(pixelCycle){
  const image=await call('render_image',{expected_revision:restored.revision,width:384,height:216,samples:16,denoise:true});
  fs.copyFileSync(path.join(dir,image.image),path.join(evidence,'restored.png'));
  pixelCycle.restored_rgb_sha256=pixels(path.join(evidence,'restored.png'));pixelCycle.restored_render=image;
  if(pixelCycle.restored_rgb_sha256!==pixelCycle.baseline_rgb_sha256)throw new Error('Undo did not restore rendered pixels');
 }
 let sequence;
 if(process.argv.includes('--sequence')){
  sequence=await call('video_export',{expected_revision:restored.revision,width:384,height:216,samples:4,denoise:true,fps:24,frames:3,format:'png-sequence'});
  if(sequence.format!=='png-sequence'||sequence.images?.length!==3||sequence.frames!==3||sequence.fps!==24)throw new Error('MCP sequence metadata mismatch');
  for(const entry of sequence.images){const file=path.join(dir,entry.image);const target=path.join(evidence,`sequence-${entry.index}.png`);fs.copyFileSync(file,target);pixels(target);}
  if((await call('scene_get')).document!==baseline.document)throw new Error('MCP sequence changed the live document');
 }
 let httpSequence;
 if(process.argv.includes('--http-sequence')){
  await client.close();
  const probe=net.createServer();await new Promise(resolve=>probe.listen(0,'127.0.0.1',resolve));const port=probe.address().port;await new Promise(resolve=>probe.close(resolve));
  httpHost=spawn(process.execPath,[path.join(runtime,'ASSISTANT/dist/http-api.js'),'--dir',dir,'--port',String(port),'--shutdown-on-stdin-close'],{windowsHide:true,stdio:['pipe','pipe','pipe']});
  await new Promise((resolve,reject)=>{let output='';const timeout=setTimeout(()=>reject(new Error('HTTP host readiness timeout')),8000);
   httpHost.stdout.on('data',chunk=>{output+=chunk;if(output.includes('HTTP API listening')){clearTimeout(timeout);resolve();}});
   httpHost.once('exit',code=>{clearTimeout(timeout);reject(new Error(`HTTP host exited before readiness: ${code}`));});});
  const token=fs.readFileSync(path.join(dir,'renderllm-http.token'),'utf8').trim();
  const http=async(route,body)=>{const response=await fetch(`http://127.0.0.1:${port}${route}`,{method:body?'POST':'GET',headers:{Authorization:`Bearer ${token}`,'Content-Type':'application/json'},...(body?{body:JSON.stringify(body)}:{}),signal:AbortSignal.timeout(180000)});const value=await response.json();if(!response.ok||value.success!==true)throw new Error(`HTTP ${route} failed (${response.status})`);return value;};
  const current=await http('/scene');
  httpSequence=await http('/video',{expected_revision:current.revision,width:384,height:216,samples:4,denoise:true,fps:24,frames:3,format:'png-sequence'});
  if(httpSequence.format!=='png-sequence'||httpSequence.images?.length!==3)throw new Error('HTTP PNG sequence metadata mismatch');
  for(const entry of httpSequence.images){const target=path.join(evidence,`http-sequence-${entry.index}.png`);fs.copyFileSync(path.join(dir,entry.image),target);pixels(target);}
  if((await http('/scene')).document!==baseline.document)throw new Error('HTTP sequence mutated scene');
 }
 const result={status:'passed',source_scene:scenePath,edit_kind:groupTransform?'group-translation':'material-color',exe_sha256:createHash('sha256').update(fs.readFileSync(exe)).digest('hex'),external_edit_observe_undo:true,menu_request_ms:menuMs,render,...(pixelCycle?{pixel_cycle:pixelCycle}:{}),...(sequence?{sequence}: {}),...(httpSequence?{http_sequence:httpSequence}: {})};
 fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify(result,null,2));console.log(JSON.stringify(result));
}finally{await client.close().catch(()=>{});if(httpHost&&httpHost.exitCode===null){httpHost.stdin.end();await new Promise(resolve=>{const timeout=setTimeout(()=>{httpHost.kill();resolve();},5000);httpHost.once('exit',()=>{clearTimeout(timeout);resolve();});});}editor.kill();}
