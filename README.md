# Banshee

This is a DRAM cache simulator implemented on top of zsim (https://github.com/s5z/zsim). The simulator supports the following DRAM cache designs. 

- Banshee [1]
- Alloy Cache
- Unison Cache
- Tagless DRAM Cache (TDC)

You can find more details in our MICRO 2017 paper:

[1] Xiangyao Yu, Chris Hughes, Nadathur Satish, Onur Mutlu, Srinivas Devadas. *Banshee: Bandwidth-Efficient DRAM Caching via Software/Hardware Cooperation*, MICRO, 2017 (http://people.csail.mit.edu/yxy/pubs/banshee.pdf)


## Usage

### Compile 

scons 

Please read the description in the zsim project (https://github.com/s5z/zsim) for details for setting the simulator.  

### Run	Test

./build/opt/zsim tests/test.cfg

## Different Cache Designs

Please read tests/test.cfg for an example configuration file. Below we summerize the parameter settings for running each DRAM cache design. 

### Banshee

mem = {  
>	...  
>	cache_scheme = "HybridCache";  
>	mcdram = {  
>>		...  
>>		cache_granularity = 4096;  
>>		num_ways = 4;  
>>		placementPolicy = "FBR";  
>>		sampleRate = 0.1;  
>>		tag_buffer_size = 1024;  
>	}  
} 

### Alloy Cache

mem = {  
>   ...  
>   cache_scheme = "AlloyCache";
>   mcdram = {  
>>		cache_granularity = 64;  
>>		num_ways = 1;  
>>		placementPolicy = "LRU";  
>	}
}

### Unison Cache 

mem = {  
>   ...  
>   cache_scheme = "UnisonCache";
>   mcdram = {  
>>		cache_granularity = 4096; 
>>		footprint_size = 64;  
>>		num_ways = 4;  
>>		placementPolicy = "LRU"; 
>>		sampleRate = 1.0;
>	}
}

### Tagless DRAM Cache (TDC) 

mem = {  
>   ...  
>   cache_scheme = "Tagless";
>   mcdram = {  
>>		cache_granularity = 4096;  
>> 		footprint_size = 64;   
>>		num_ways = size * 1024 * 1024 / lineSize; 
>	}
}
