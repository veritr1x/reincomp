import {WindowsToolchain} from "./toolchain.mjs";

const tools = new WindowsToolchain();
let library;
self.onmessage = async ({data}) => {
  try {
    if (data.library) {
      library = data.library;
      self.postMessage({ready:true});
      return;
    }
    const {object, targets} = await tools.shard({...library, entries:data.entries, optimization:data.optimization,
      binaryHeads:true, pollFlags:["--speed","--inline-poll=teb","--direct-calls"]});
    self.postMessage({object,targets});
  } catch (error) {
    self.postMessage({error:String(error.stack || error)});
  }
};
